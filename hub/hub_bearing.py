"""Пересечение пеленгов для трека Hub.

Два узла: единственная точка двух лучей (планиметрия).
Три и больше: старт — псевдолинейная оценка (перпендикуляры к лучам),
затем Гаусс–Ньютон по угловым невязкам. Это максимум правдоподобия при
нормальном шуме пеленга. Число узлов и их расстановка заранее неизвестны,
поэтому метрический компромисс не оставляем финалом: дальний узел иначе
перетягивает точку длинным перпендикуляром. Два узла и так дают одну точку.

Луч, не прямая: точка обязана лежать впереди каждого массива (t > 0).
Почти параллельные лучи отбрасываем — их пересечение неустойчиво.
"""
from __future__ import annotations

import math
from typing import Any

MIN_BASELINE_M = 40.0
ANGLE_MIN_DEG = 20.0
ANGLE_MAX_DEG = 160.0
ENVELOPE_SLACK = 1.15
SIGMA_DEG = 12.0
RESIDUAL_FRAC = 0.35
_M_PER_DEG_LAT = 111320.0


def bearing_intersect(nodes: list[dict[str, Any]]) -> dict[str, Any] | None:
    """lat/lon пересечения или None, если геометрия не годится для точки."""
    rays = [_prepare(n) for n in nodes]
    usable = [r for r in rays if r is not None]
    if len(usable) < 2:
        return None
    solved = _solve(usable)
    if solved is None:
        return None
    east, north, ranges, method, kept, lat0, lon0, mlat, mlon = solved
    baseline = _max_baseline(kept)
    if baseline < MIN_BASELINE_M:
        return None
    angle, pair = _best_angle(kept)
    if angle is None or angle < ANGLE_MIN_DEG or angle > ANGLE_MAX_DEG:
        return None
    lat = lat0 + north / mlat
    lon = lon0 + east / mlon
    if not math.isfinite(lat) or not math.isfinite(lon):
        return None
    sin_a = max(math.sin(math.radians(angle)), 0.02)
    return {
        "lat": lat,
        "lon": lon,
        "method": method,
        "gdop": round(1.0 / sin_a, 2),
        "baseline_m": round(baseline, 1),
        "crossing_deg": round(angle, 1),
        "ellipse": _ellipse(ranges, angle, pair),
    }


def _prepare(node: dict[str, Any]) -> dict[str, Any] | None:
    if not isinstance(node, dict):
        return None
    try:
        lat = float(node["lat"])
        lon = float(node["lon"])
        azimuth = float(node["azimuth_deg"])
        radius = float(node["r_m"])
        trust_raw = node.get("trust")
        trust = float(1.0 if trust_raw is None else trust_raw)
    except (KeyError, TypeError, ValueError):
        return None
    if not all(math.isfinite(v) for v in (lat, lon, azimuth, radius, trust)):
        return None
    if radius <= 0 or trust <= 0:
        return None
    return {
        "node_id": str(node.get("node_id") or ""),
        "lat": lat,
        "lon": lon,
        "azimuth_deg": azimuth % 360.0,
        "r_m": radius,
        "trust": trust,
    }


def _solve(
    nodes: list[dict[str, Any]],
) -> tuple[float, float, list[float], str, list[dict[str, Any]], float, float, float, float] | None:
    lat0 = sum(n["lat"] for n in nodes) / len(nodes)
    lon0 = sum(n["lon"] for n in nodes) / len(nodes)
    mlat = _M_PER_DEG_LAT
    mlon = _M_PER_DEG_LAT * math.cos(math.radians(lat0))
    if mlon <= 1.0:
        return None
    local = [_to_local(n, lat0, lon0, mlat, mlon) for n in nodes]
    if len(local) == 2:
        hit = _analytic(local[0], local[1])
        if hit is None:
            return None
        east, north, ranges = hit
        return east, north, ranges, "bearing_intersection", local, lat0, lon0, mlat, mlon
    draft = _best_pair_point(local)
    if draft is None:
        return None
    east, north, _pair = draft
    kept = []
    for ray in local:
        if _ray_ok(ray, east, north):
            kept.append(ray)
    if len(kept) < 2:
        return None
    if len(kept) == 2:
        return _solve(kept)
    point = _lsq(kept)
    if point is None:
        return None
    east, north = point
    still = [ray for ray in kept if _ray_ok(ray, east, north)]
    if len(still) < 2:
        return None
    if len(still) < len(kept):
        return _solve(still)
    refined = _mle(still, east, north)
    if refined is not None and all(_ray_ok(ray, refined[0], refined[1]) for ray in still):
        east, north = refined
        method = "bearing_mle"
    else:
        method = "bearing_lsq"
    ranges = [_along(ray, east, north) for ray in still]
    return east, north, ranges, method, still, lat0, lon0, mlat, mlon


def _to_local(
    node: dict[str, Any], lat0: float, lon0: float, mlat: float, mlon: float
) -> dict[str, Any]:
    rad = math.radians(node["azimuth_deg"])
    out = dict(node)
    out["e"] = (node["lon"] - lon0) * mlon
    out["n"] = (node["lat"] - lat0) * mlat
    out["de"] = math.sin(rad)
    out["dn"] = math.cos(rad)
    return out


def _analytic(
    a: dict[str, Any], b: dict[str, Any]
) -> tuple[float, float, list[float]] | None:
    cross = a["de"] * b["dn"] - a["dn"] * b["de"]
    if abs(cross) < 1e-6:
        return None
    de_p = b["e"] - a["e"]
    dn_p = b["n"] - a["n"]
    t_a = (de_p * b["dn"] - dn_p * b["de"]) / cross
    t_b = (de_p * a["dn"] - dn_p * a["de"]) / cross
    if t_a <= 0.0 or t_b <= 0.0:
        return None
    if t_a > ENVELOPE_SLACK * a["r_m"] or t_b > ENVELOPE_SLACK * b["r_m"]:
        return None
    return a["e"] + t_a * a["de"], a["n"] + t_a * a["dn"], [t_a, t_b]


def _best_pair_point(
    rays: list[dict[str, Any]],
) -> tuple[float, float, tuple[dict[str, Any], dict[str, Any]]] | None:
    best: tuple[float, float, tuple[dict[str, Any], dict[str, Any]]] | None = None
    best_gdop = 1e9
    for i, a in enumerate(rays):
        for b in rays[i + 1 :]:
            hit = _analytic(a, b)
            if hit is None:
                continue
            angle = _angle_deg(a, b)
            if angle < ANGLE_MIN_DEG or angle > ANGLE_MAX_DEG:
                continue
            gdop = 1.0 / max(math.sin(math.radians(angle)), 0.02)
            if gdop < best_gdop:
                best_gdop = gdop
                best = (hit[0], hit[1], (a, b))
    return best


def _lsq(rays: list[dict[str, Any]]) -> tuple[float, float] | None:
    sxx = sxy = syy = sx = sy = 0.0
    for ray in rays:
        weight = ray["trust"]
        de, dn = ray["de"], ray["dn"]
        px = (1.0 - de * de) * weight
        pxy = (-de * dn) * weight
        py = (1.0 - dn * dn) * weight
        sxx += px
        sxy += pxy
        syy += py
        sx += px * ray["e"] + pxy * ray["n"]
        sy += pxy * ray["e"] + py * ray["n"]
    det = sxx * syy - sxy * sxy
    if abs(det) < 1e-8:
        return None
    return (sx * syy - sy * sxy) / det, (sxx * sy - sxy * sx) / det


def _mle(
    rays: list[dict[str, Any]], east: float, north: float
) -> tuple[float, float] | None:
    """Гаусс–Ньютон: минимум суммы квадратов угловых невязок."""
    e, n = east, north
    sigma2 = math.radians(SIGMA_DEG) ** 2
    for _ in range(12):
        jtj_ee = jtj_en = jtj_nn = 0.0
        jtr_e = jtr_n = 0.0
        for ray in rays:
            de = e - ray["e"]
            dn = n - ray["n"]
            r2 = de * de + dn * dn
            if r2 < 1.0:
                return None
            pred = math.atan2(de, dn)
            meas = math.atan2(ray["de"], ray["dn"])
            resid = (meas - pred + math.pi) % (2.0 * math.pi) - math.pi
            je = dn / r2
            jn = -de / r2
            weight = ray["trust"] / sigma2
            jtj_ee += weight * je * je
            jtj_en += weight * je * jn
            jtj_nn += weight * jn * jn
            jtr_e += weight * je * resid
            jtr_n += weight * jn * resid
        det = jtj_ee * jtj_nn - jtj_en * jtj_en
        if abs(det) < 1e-18:
            return None
        step_e = (jtr_e * jtj_nn - jtr_n * jtj_en) / det
        step_n = (jtj_ee * jtr_n - jtj_en * jtr_e) / det
        step = math.hypot(step_e, step_n)
        if step > 200.0:
            step_e *= 200.0 / step
            step_n *= 200.0 / step
            step = 200.0
        e += step_e
        n += step_n
        if not math.isfinite(e) or not math.isfinite(n):
            return None
        if step < 0.3:
            break
    return e, n


def _along(ray: dict[str, Any], east: float, north: float) -> float:
    return (east - ray["e"]) * ray["de"] + (north - ray["n"]) * ray["dn"]


def _perp(ray: dict[str, Any], east: float, north: float) -> float:
    return abs((east - ray["e"]) * ray["dn"] - (north - ray["n"]) * ray["de"])


def _ray_ok(ray: dict[str, Any], east: float, north: float) -> bool:
    along = _along(ray, east, north)
    if along <= 0.0 or along > ENVELOPE_SLACK * ray["r_m"]:
        return False
    return _perp(ray, east, north) <= RESIDUAL_FRAC * ray["r_m"]


def _angle_deg(a: dict[str, Any], b: dict[str, Any]) -> float:
    cross = abs(a["de"] * b["dn"] - a["dn"] * b["de"])
    dot = a["de"] * b["de"] + a["dn"] * b["dn"]
    return math.degrees(math.atan2(cross, dot))


def _best_angle(
    rays: list[dict[str, Any]],
) -> tuple[float | None, tuple[dict[str, Any], dict[str, Any]] | None]:
    best_sin = -1.0
    best: tuple[float, tuple[dict[str, Any], dict[str, Any]]] | None = None
    for i, a in enumerate(rays):
        for b in rays[i + 1 :]:
            angle = _angle_deg(a, b)
            sine = math.sin(math.radians(angle))
            if sine > best_sin:
                best_sin = sine
                best = (angle, (a, b))
    if best is None:
        return None, None
    return best


def _max_baseline(rays: list[dict[str, Any]]) -> float:
    best = 0.0
    for i, a in enumerate(rays):
        for b in rays[i + 1 :]:
            dist = _haversine_m(a["lat"], a["lon"], b["lat"], b["lon"])
            if dist > best:
                best = dist
    return best


def _haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    radius = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlmb = math.radians(lon2 - lon1)
    h = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlmb / 2) ** 2
    return 2 * radius * math.asin(min(1.0, math.sqrt(h)))


def _ellipse(
    ranges: list[float],
    angle_deg: float,
    pair: tuple[dict[str, Any], dict[str, Any]] | None,
) -> dict[str, float]:
    mean_r = sum(ranges) / len(ranges)
    cross = mean_r * math.tan(math.radians(SIGMA_DEG))
    cross = _clamp(cross, 30.0, 800.0)
    along = cross / max(math.sin(math.radians(angle_deg)), 0.15)
    along = _clamp(along, 30.0, 800.0)
    major = 0.0
    if pair is not None:
        de = pair[0]["de"] + pair[1]["de"]
        dn = pair[0]["dn"] + pair[1]["dn"]
        if abs(de) + abs(dn) > 1e-9:
            major = math.degrees(math.atan2(de, dn)) % 360.0
    return {
        "cross_m": round(cross, 1),
        "along_m": round(along, 1),
        "major_bearing_deg": round(major, 1),
    }


def _clamp(value: float, lo: float, hi: float) -> float:
    return lo if value < lo else hi if value > hi else value
