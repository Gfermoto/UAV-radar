"""Слияние детекций узлов в трек вероятного БПЛА.

Одно наблюдение на node_id. Пара слышит одну цель, если оба класса из
семьи БПЛА, они уложились в окно и стоят ближе суммы радиусов ISO 9613.
Точку считает hub_bearing. JS её только рисует.
"""
from __future__ import annotations

import math
import uuid
from typing import Any

from hub_bearing import bearing_intersect
from hub_iso9613 import envelope_at

UAV_CLASSES = frozenset({"drone", "ice_uav", "jet_uav"})
LABEL_RU = {
    "drone": "БПЛА",
    "ice_uav": "поршневой",
    "jet_uav": "реактивный",
}
WINDOW_WIFI_MS = 8000
WINDOW_LORA_MS = 20000
OBS_MAX_AGE_MS = 20000
STALE_MS = 20000
DROP_MS = 45000
STICK_M = 180.0
STICK_MS = 15000
CLUSTER_M = 120.0
PB_PREFER_MS = 2000
CIVIL_DEFAULT_M = 3000.0


def hearing_radius_m(
    lat: float, lon: float, azimuth_deg: float, wx: dict[str, Any] | None
) -> float:
    """Изотропный радиус ISO. Нет погоды — опорные 20 °C / 70 % / 101.325 кПа."""
    _ = (lat, lon)
    ctx: dict[str, Any] = {}
    if isinstance(wx, dict):
        t_c = _finite(wx.get("t_c"))
        rh = _finite(wx.get("rh_pct"))
        pressure = _finite(wx.get("p_kpa"))
        if t_c is not None and rh is not None and pressure is not None:
            ctx = {"t_c": t_c, "rh_pct": rh, "p_kpa": pressure, "f_hz": 500.0}
    return float(envelope_at(float(azimuth_deg), ctx)["r_m"])


def observation_from_detection(
    det: dict[str, Any],
    node_lat: Any,
    node_lon: Any,
    now_ms: int,
) -> dict[str, Any] | None:
    """Плоское наблюдение. Нет азимута — None. Азимут 0° остаётся севером."""
    if not isinstance(det, dict):
        return None
    azimuth = det.get("azimuth_deg")
    bearing = det.get("bearing")
    if azimuth is None and isinstance(bearing, dict):
        azimuth = bearing.get("azimuth_deg")
    azimuth_f = _finite(azimuth)
    if azimuth_f is None:
        return None
    lat = _finite(det.get("lat"))
    lon = _finite(det.get("lon"))
    if lat is None or lon is None:
        lat = _finite(node_lat)
        lon = _finite(node_lon)
    if lat is None or lon is None:
        return None
    ts = _finite(det.get("timestamp_ms"))
    if ts is None:
        ts = _finite(det.get("ts_ms"))
    if ts is None:
        ts = float(now_ms)
    via = _norm_via(det.get("via"))
    cls = str(det.get("class_name") or det.get("class") or "").strip().lower()
    prob = _finite(det.get("p"))
    if prob is None:
        prob = _finite(det.get("confidence")) or 0.0
    threat = _finite(det.get("threat")) or 0.0
    wx = det.get("wx") if isinstance(det.get("wx"), dict) else None
    doa = _finite(det.get("doa_confidence"))
    return {
        "node_id": str(det.get("node_id") or ""),
        "ts_ms": int(ts),
        "lat": lat,
        "lon": lon,
        "azimuth_deg": azimuth_f,
        "class_name": cls,
        "p": prob,
        "threat": threat,
        "via": via,
        "doa_confidence": doa,
        "wx": wx,
        "trust": _trust(via, doa),
        "r_m": hearing_radius_m(lat, lon, azimuth_f, wx),
    }


def civil_aircraft_from_adsb(hub_adsb: dict[str, Any] | None) -> list[dict[str, float]]:
    """Борты in_radius живого фида. Радиус пузыря — radius_m зоны, иначе 3 км."""
    if not isinstance(hub_adsb, dict) or not hub_adsb.get("fresh"):
        return []
    radius = _finite(hub_adsb.get("radius_m"))
    if radius is None or radius <= 0:
        radius = CIVIL_DEFAULT_M
    out: list[dict[str, float]] = []
    for ac in hub_adsb.get("aircraft") or []:
        if not isinstance(ac, dict) or not ac.get("in_radius"):
            continue
        lat = _finite(ac.get("lat"))
        lon = _finite(ac.get("lon"))
        if lat is None or lon is None:
            continue
        out.append({"lat": lat, "lon": lon, "radius_m": radius})
    return out


class TrackEngine:
    def __init__(self) -> None:
        self._obs: dict[str, dict[str, Any]] = {}
        self._tracks: dict[str, dict[str, Any]] = {}
        self._pending_drops: list[str] = []
        self._civil: list[dict[str, Any]] = []

    def ingest(self, obs: dict[str, Any], *, now_ms: int) -> None:
        _ = now_ms
        if not isinstance(obs, dict):
            return
        node_id = str(obs.get("node_id") or "")
        if not node_id:
            return
        prev = self._obs.get(node_id)
        if prev is not None and _keep_previous(prev, obs):
            return
        stored = dict(obs)
        if _finite(stored.get("r_m")) is None:
            stored["r_m"] = hearing_radius_m(
                float(stored["lat"]),
                float(stored["lon"]),
                float(stored["azimuth_deg"]),
                stored.get("wx") if isinstance(stored.get("wx"), dict) else None,
            )
        if _finite(stored.get("trust")) is None:
            stored["trust"] = _trust(str(stored.get("via") or ""), _finite(stored.get("doa_confidence")))
        self._obs[node_id] = stored

    def set_civil(self, aircraft: list[dict[str, Any]] | None) -> None:
        self._civil = list(aircraft or [])

    def snapshot(self, now_ms: int) -> list[dict[str, Any]]:
        self._expire(now_ms)
        clusters = _clusters(self._live(now_ms))
        refreshed: set[str] = set()
        for members, hit in clusters:
            track = self._stick(hit, now_ms)
            track["last_ms"] = now_ms
            track["lat"] = hit["lat"]
            track["lon"] = hit["lon"]
            track["hit"] = hit
            track["members"] = members
            refreshed.add(track["id"])
        rows: list[dict[str, Any]] = []
        for tid, track in list(self._tracks.items()):
            age = now_ms - int(track["last_ms"])
            if tid not in refreshed and age > DROP_MS:
                self._drop(tid)
                continue
            if tid not in refreshed and age > STALE_MS:
                state = "stale"
            else:
                state = "active"
            rows.append(self._public(track, state))
        return rows

    def dropped_ids(self, now_ms: int) -> list[str]:
        self._expire(now_ms)
        out = list(self._pending_drops)
        self._pending_drops.clear()
        return out

    def _stick(self, hit: dict[str, Any], now_ms: int) -> dict[str, Any]:
        best = None
        best_d = 1e18
        for track in self._tracks.values():
            if now_ms - int(track["last_ms"]) > STICK_MS:
                continue
            dist = _haversine_m(hit["lat"], hit["lon"], track["lat"], track["lon"])
            if dist <= STICK_M and dist < best_d:
                best = track
                best_d = dist
        if best is not None:
            return best
        tid = "trk_" + uuid.uuid4().hex[:12]
        track = {"id": tid, "first_ms": now_ms, "last_ms": now_ms}
        self._tracks[tid] = track
        return track

    def _expire(self, now_ms: int) -> None:
        for tid, track in list(self._tracks.items()):
            if now_ms - int(track["last_ms"]) > DROP_MS:
                self._drop(tid)

    def _drop(self, tid: str) -> None:
        if tid not in self._tracks:
            return
        del self._tracks[tid]
        if tid not in self._pending_drops:
            self._pending_drops.append(tid)

    def _live(self, now_ms: int) -> list[dict[str, Any]]:
        rows = []
        for obs in self._obs.values():
            if now_ms - int(obs["ts_ms"]) > OBS_MAX_AGE_MS:
                continue
            if obs.get("class_name") not in UAV_CLASSES:
                continue
            rows.append(obs)
        return rows

    def _public(self, track: dict[str, Any], state: str) -> dict[str, Any]:
        members: list[dict[str, Any]] = list(track.get("members") or [])
        hit: dict[str, Any] = dict(track.get("hit") or {})
        votes: dict[str, list[float]] = {}
        threat = 0.0
        for obs in members:
            cls = str(obs.get("class_name") or "")
            votes.setdefault(cls, []).append(float(obs.get("p") or 0.0))
            threat = max(threat, float(obs.get("threat") or 0.0))
        winner = ""
        winner_p = 0.0
        if votes:
            winner = max(votes, key=lambda k: (sum(votes[k]) / len(votes[k]), k))
            winner_p = sum(votes[winner]) / len(votes[winner])
        alternatives = []
        for cls, samples in votes.items():
            if cls == winner:
                continue
            alternatives.append({"class": cls, "p": round(sum(samples) / len(samples), 3)})
        alternatives.sort(key=lambda row: row["p"], reverse=True)
        quality = "low" if len(votes) > 1 or float(hit.get("gdop") or 0) > 8.0 else "medium"
        estimate = {
            "method": hit.get("method"),
            "quality": quality,
            "lat": hit.get("lat"),
            "lon": hit.get("lon"),
            "gdop": hit.get("gdop"),
            "baseline_m": hit.get("baseline_m"),
            "crossing_deg": hit.get("crossing_deg"),
            "ellipse": hit.get("ellipse"),
        }
        lat = _finite(hit.get("lat"))
        lon = _finite(hit.get("lon"))
        civil = lat is not None and lon is not None and _near_civil(lat, lon, self._civil)
        return {
            "schema": "nevod.track.v1",
            "track_id": track["id"],
            "state": state,
            "first_seen_ms": int(track["first_ms"]),
            "last_seen_ms": int(track["last_ms"]),
            "target": {
                "class": winner,
                "label_ru": LABEL_RU.get(winner, winner),
                "alternatives": alternatives,
            },
            "p": round(winner_p, 3),
            "threat": round(threat, 3),
            "estimate": estimate,
            "contributors": [
                {
                    "node_id": obs["node_id"],
                    "azimuth_deg": obs["azimuth_deg"],
                    "lat": obs["lat"],
                    "lon": obs["lon"],
                    "via": obs["via"],
                    "class": obs["class_name"],
                    "p": round(float(obs["p"]), 3),
                    "ts_ms": obs["ts_ms"],
                }
                for obs in members
            ],
            "n_nodes": len(members),
            "civil_possible": civil,
        }


def _keep_previous(prev: dict[str, Any], new: dict[str, Any]) -> bool:
    """True — оставить prev. PB в окне 2 с не вытесняется MQTT."""
    try:
        dt = abs(int(new["ts_ms"]) - int(prev["ts_ms"]))
    except (KeyError, TypeError, ValueError):
        return False
    if dt <= PB_PREFER_MS and prev.get("via") == "pb" and new.get("via") != "pb":
        return True
    if dt <= PB_PREFER_MS and new.get("via") == "pb":
        return False
    return int(new.get("ts_ms") or 0) < int(prev.get("ts_ms") or 0)


def _clusters(
    live: list[dict[str, Any]],
) -> list[tuple[list[dict[str, Any]], dict[str, Any]]]:
    pairs: list[tuple[float, dict[str, Any], dict[str, Any], dict[str, Any]]] = []
    for i, a in enumerate(live):
        for b in live[i + 1 :]:
            hit = _pair_hit(a, b)
            if hit is None:
                continue
            pairs.append((float(hit["gdop"]), a, b, hit))
    pairs.sort(key=lambda row: row[0])
    used: set[str] = set()
    clusters: list[tuple[list[dict[str, Any]], dict[str, Any]]] = []
    for _gdop, a, b, hit in pairs:
        if a["node_id"] in used or b["node_id"] in used:
            continue
        members = [a, b]
        point = hit
        used.add(a["node_id"])
        used.add(b["node_id"])
        grew = True
        while grew:
            grew = False
            for extra in live:
                if extra["node_id"] in used:
                    continue
                if not _can_join(extra, members):
                    continue
                trial = members + [extra]
                hit_n = bearing_intersect([_ray(obs) for obs in trial])
                if hit_n is None:
                    continue
                if _haversine_m(hit_n["lat"], hit_n["lon"], point["lat"], point["lon"]) > CLUSTER_M:
                    continue
                members = trial
                point = hit_n
                used.add(extra["node_id"])
                grew = True
        clusters.append((members, point))
    return clusters


def _can_join(extra: dict[str, Any], members: list[dict[str, Any]]) -> bool:
    for obs in members:
        if abs(int(extra["ts_ms"]) - int(obs["ts_ms"])) > _window_ms(extra, obs):
            return False
        if _haversine_m(extra["lat"], extra["lon"], obs["lat"], obs["lon"]) > extra["r_m"] + obs["r_m"]:
            return False
    return True


def _pair_hit(a: dict[str, Any], b: dict[str, Any]) -> dict[str, Any] | None:
    if abs(int(a["ts_ms"]) - int(b["ts_ms"])) > _window_ms(a, b):
        return None
    if _haversine_m(a["lat"], a["lon"], b["lat"], b["lon"]) > float(a["r_m"]) + float(b["r_m"]):
        return None
    return bearing_intersect([_ray(a), _ray(b)])


def _ray(obs: dict[str, Any]) -> dict[str, Any]:
    return {
        "node_id": obs["node_id"],
        "lat": obs["lat"],
        "lon": obs["lon"],
        "azimuth_deg": obs["azimuth_deg"],
        "r_m": obs["r_m"],
        "trust": obs["trust"],
    }


def _window_ms(a: dict[str, Any], b: dict[str, Any]) -> int:
    if a.get("via") == "lora_gw" or b.get("via") == "lora_gw":
        return WINDOW_LORA_MS
    return WINDOW_WIFI_MS


def _trust(via: str, doa: float | None) -> float:
    trust = 1.0 if doa is not None and doa > 0 else 0.6
    if via == "lora_gw":
        trust *= 0.7
    return trust


def _norm_via(via: Any) -> str:
    text = str(via or "")
    if text in ("pb", "pb_detection"):
        return "pb"
    return text or "mqtt_detection"


def _near_civil(lat: float, lon: float, aircraft: list[dict[str, Any]]) -> bool:
    for ac in aircraft:
        alat = _finite(ac.get("lat"))
        alon = _finite(ac.get("lon"))
        if alat is None or alon is None:
            continue
        radius = _finite(ac.get("radius_m"))
        if radius is None or radius <= 0:
            radius = CIVIL_DEFAULT_M
        if _haversine_m(lat, lon, alat, alon) <= radius:
            return True
    return False


def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    radius = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlmb = math.radians(lon2 - lon1)
    h = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlmb / 2) ** 2
    return 2 * radius * math.asin(min(1.0, math.sqrt(h)))


def _finite(value: Any) -> float | None:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(number):
        return None
    return number
