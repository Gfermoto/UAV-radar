"""DEM helpers for Hub map (OpenTopoData public + ISA pressure)."""
from __future__ import annotations

import json
import math
import threading
import time
import urllib.error
import urllib.request
from typing import Any

_LOCK = threading.Lock()
_CACHE: dict[str, tuple[float, list[float | None]]] = {}
_CACHE_TTL_S = 6 * 3600.0
_TIMEOUT_S = 10.0
_UA = "NEVOD-Hub/0.1.66 (lab; dem; +https://github.com/Gfermoto/nevod)"

# Per-point tip cache — snapshot must NOT block on OpenTopo.
_POINT_ELEV: dict[str, tuple[float, float | None]] = {}
_POINT_TTL_S = 6 * 3600.0
_POINT_MISS_TTL_S = 5 * 60.0  # transient OpenTopo fail → retry sooner
_PENDING: set[str] = set()
_BG_CV = threading.Condition()
_BG_STARTED = False


def isa_pressure_kpa(p_sea_hpa: float, elev_m: float) -> float:
    """International Standard Atmosphere: sea-level hPa → local kPa."""
    h = float(elev_m)
    p0 = float(p_sea_hpa) / 10.0  # hPa → kPa
    return p0 * (1.0 - 2.25577e-5 * h) ** 5.25588


def _post_locations(locations: str) -> dict[str, Any]:
    url = "https://api.opentopodata.org/v1/srtm30m,mapzen"
    body = json.dumps({"locations": locations}).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={
            "Accept": "application/json",
            "Content-Type": "application/json",
            "User-Agent": _UA,
        },
    )
    with urllib.request.urlopen(req, timeout=_TIMEOUT_S) as resp:
        raw = resp.read()
    obj = json.loads(raw.decode("utf-8"))
    if not isinstance(obj, dict):
        raise ValueError("bad_json")
    return obj


def fetch_elevations(points: list[tuple[float, float]]) -> list[float | None]:
    """Query OpenTopoData; max 100 points per request (public API)."""
    if not points:
        return []
    if len(points) > 100:
        points = points[:100]
    key = "|".join(f"{la:.4f},{lo:.4f}" for la, lo in points)
    cached = _CACHE.get(key)
    if cached and time.time() - cached[0] < _CACHE_TTL_S:
        return list(cached[1])
    locations = "|".join(f"{la:.5f},{lo:.5f}" for la, lo in points)
    try:
        j = _post_locations(locations)
    except urllib.error.HTTPError:
        return [None] * len(points)
    except Exception:
        return [None] * len(points)
    results = j.get("results") or []
    out: list[float | None] = []
    for i in range(len(points)):
        if i < len(results) and isinstance(results[i], dict):
            el = results[i].get("elevation")
            out.append(float(el) if el is not None else None)
        else:
            out.append(None)
    with _LOCK:
        _CACHE[key] = (time.time(), out)
    return out


def ring_points(lat: float, lon: float, sample_m: float, n: int = 16) -> list[tuple[float, float]]:
    """Center + n points on a ring at sample_m (same geometry as hub_meteo)."""
    pts = [(lat, lon)]
    r = 6371000.0
    for i in range(n):
        br = i * (360.0 / n) * math.pi / 180.0
        d = max(50.0, float(sample_m)) / r
        φ1 = lat * math.pi / 180.0
        λ1 = lon * math.pi / 180.0
        φ2 = math.asin(
            math.sin(φ1) * math.cos(d) + math.cos(φ1) * math.sin(d) * math.cos(br)
        )
        λ2 = λ1 + math.atan2(
            math.sin(br) * math.sin(d) * math.cos(φ1),
            math.cos(d) - math.sin(φ1) * math.sin(φ2),
        )
        pts.append((φ2 * 180.0 / math.pi, ((λ2 * 180.0 / math.pi + 540.0) % 360.0) - 180.0))
    return pts


def proxy_dem(
    lat: float,
    lon: float,
    sample_m: float = 500.0,
    n: int = 16,
) -> tuple[int, dict[str, Any]]:
    if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
        return 400, {"error": "bad_coord"}
    if lat == 0.0 and lon == 0.0:
        return 400, {"error": "bad_coord"}
    pts = ring_points(lat, lon, sample_m, n=n)
    elev = fetch_elevations(pts)
    if not elev or elev[0] is None:
        return 502, {"error": "dem_empty"}
    return 200, {"elev_m": elev, "source": "opentopodata", "sample_m": sample_m}


def _install_agl_m(hb: dict[str, Any]) -> float | None:
    """Install height AGL from HB; legacy alt_m only if AGL-sized (≤40)."""
    raw = hb.get("install_height_m")
    if raw is None:
        pos = hb.get("node_position")
        if isinstance(pos, dict):
            raw = pos.get("install_height_m")
    try:
        if raw is not None:
            v = float(raw)
            if 0.3 <= v <= 80.0:
                return v
    except (TypeError, ValueError):
        pass
    try:
        alt = hb.get("alt_m")
        if alt is None and isinstance(hb.get("node_position"), dict):
            alt = hb["node_position"].get("alt_m")
        if alt is not None:
            v = float(alt)
            if 0.3 <= v <= 40.0:
                return v
    except (TypeError, ValueError):
        pass
    return None


def _coord_plausible(lat: float, lon: float) -> bool:
    if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
        return False
    if lat == 0.0 and lon == 0.0:
        return False
    # Placeholder / unset (e.g. lat=90) — не долбить OpenTopo.
    if abs(lat) >= 89.0:
        return False
    return True


def _point_key(lat: float, lon: float) -> str:
    return f"{round(lat, 4):.4f},{round(lon, 4):.4f}"


def _elev_sane(el: float | None) -> float | None:
    if el is None:
        return None
    try:
        v = float(el)
    except (TypeError, ValueError):
        return None
    # SRTM void / garbage (pole etc.)
    if v < -500.0 or v > 9000.0:
        return None
    return v


def _cached_point_elev(lat: float, lon: float) -> tuple[float | None, bool]:
    """→ (elev_or_None, cache_hit)."""
    k = _point_key(lat, lon)
    with _LOCK:
        hit = _POINT_ELEV.get(k)
    if not hit:
        return None, False
    ts, elev = hit
    age = time.time() - ts
    ttl = _POINT_MISS_TTL_S if elev is None else _POINT_TTL_S
    if age < ttl:
        return elev, True
    return None, False


def _ensure_bg() -> None:
    global _BG_STARTED
    if _BG_STARTED:
        return
    with _BG_CV:
        if _BG_STARTED:
            return
        _BG_STARTED = True
        threading.Thread(target=_bg_loop, name="hub-dem-tips", daemon=True).start()


def _schedule_point(lat: float, lon: float) -> None:
    k = _point_key(lat, lon)
    with _BG_CV:
        hit = _POINT_ELEV.get(k)
        if hit:
            ts, elev = hit
            ttl = _POINT_MISS_TTL_S if elev is None else _POINT_TTL_S
            if time.time() - ts < ttl:
                return
        pending_add = k not in _PENDING
        if pending_add:
            _PENDING.add(k)
        _ensure_bg()
        if pending_add:
            _BG_CV.notify()


def _bg_loop() -> None:
    while True:
        with _BG_CV:
            while not _PENDING:
                _BG_CV.wait(timeout=60.0)
            batch_keys = list(_PENDING)[:40]
            for k in batch_keys:
                _PENDING.discard(k)
        pts: list[tuple[float, float]] = []
        keys: list[str] = []
        for k in batch_keys:
            try:
                la_s, lo_s = k.split(",", 1)
                la, lo = float(la_s), float(lo_s)
            except (TypeError, ValueError):
                continue
            if not _coord_plausible(la, lo):
                now = time.time()
                with _LOCK:
                    _POINT_ELEV[k] = (now, None)
                continue
            pts.append((la, lo))
            keys.append(k)
        if not pts:
            continue
        elevs = fetch_elevations(pts)
        now = time.time()
        with _LOCK:
            for i, k in enumerate(keys):
                el = _elev_sane(elevs[i] if i < len(elevs) else None)
                _POINT_ELEV[k] = (now, el)


def warm_point_elev(lat: float, lon: float, elev_m: float | None) -> None:
    """Test/helper: seed per-point cache without network."""
    k = _point_key(lat, lon)
    with _LOCK:
        _POINT_ELEV[k] = (time.time(), _elev_sane(elev_m))


def attach_node_tips(nodes: list[dict[str, Any]]) -> None:
    """Snapshot: ground_elev_m + tip_alt_m from cache only (DEM bg)."""
    for n in nodes:
        if not isinstance(n, dict):
            continue
        hb = n.get("last_heartbeat")
        if not isinstance(hb, dict):
            hb = {}
        try:
            la = float(hb.get("lat"))
            lo = float(hb.get("lon"))
        except (TypeError, ValueError):
            continue
        if not _coord_plausible(la, lo):
            continue
        elev, hit = _cached_point_elev(la, lo)
        if not hit:
            _schedule_point(la, lo)
            continue
        if elev is None:
            continue
        ground = round(float(elev), 1)
        n["ground_elev_m"] = ground
        ih = _install_agl_m(hb)
        if ih is not None:
            n["install_height_m"] = ih
            n["tip_alt_m"] = round(ground + ih, 1)
