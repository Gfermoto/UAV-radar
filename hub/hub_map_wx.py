"""Visual Hub map envelope — thin wrapper over hub_iso9613 (ISO 9613-1/2).

NOT DET range. Wind/day/precip do not multiply radius.
See docs/superpowers/plans/2026-09-20-hub-iso9613-envelope.md
"""
from __future__ import annotations

from typing import Any

from hub_iso9613 import (
    A_DIV_REF_M,
    DEFAULT_BUILDING_H_M,
    F_REF_HZ,
    HR_M,
    barrier_db,
    barrier_path_diff_m,
    envelope_at as _envelope_at,
    foliage_db,
    ground_extra_db,
    range_m,
)

# Re-exports used by older imports / JS lockstep naming
BASE_R_M = A_DIV_REF_M


def foliage_atten_db(path_m: float, f_hz: float = F_REF_HZ) -> float:
    return foliage_db(path_m, f_hz)


def foliage_scale(path_m: float, f_hz: float = F_REF_HZ) -> float:
    """Legacy amplitude scale — prefer envelope_at."""
    return 10.0 ** (-foliage_db(path_m, f_hz) / 20.0)


def envelope_at(bearing_deg: float, ctx: dict[str, Any]) -> dict[str, Any]:
    return _envelope_at(bearing_deg, ctx)


def radius_at(
    base_m: float,
    bearing_deg: float,
    *,
    t_c: float | None = None,
    rh_pct: float | None = None,
    p_hpa: float | None = None,
    precip_mm: float | None = None,
    wind_from_deg: float | None = None,
    wind_ms: float | None = None,
    gust_ms: float | None = None,
    d_elev_m: float | None = None,
    is_day: bool | None = None,
    twilight: float | None = None,
    sunrise_iso: str | None = None,
    sunset_iso: str | None = None,
    now_s: float | None = None,
    forest_path_m: float = 0.0,
    building_blocked: bool = False,
    building_dist_m: float = 0.0,
    g: float = 1.0,
    barrier_z_m: float = 0.0,
    f_hz: float = F_REF_HZ,
    hr_m: float | None = None,
    hs_m: float | None = None,
) -> float:
    """ISO envelope radius. Legacy kwargs (wind/day/precip) ignored on purpose."""
    _ = (
        base_m,
        precip_mm,
        wind_from_deg,
        wind_ms,
        gust_ms,
        d_elev_m,
        is_day,
        twilight,
        sunrise_iso,
        sunset_iso,
        now_s,
    )
    p_kpa = 101.325 if p_hpa is None else float(p_hpa) / 10.0
    hr = float(HR_M if hr_m is None else hr_m)
    z = float(barrier_z_m)
    if z <= 0 and building_blocked:
        # OSM without height → suburban default 2 floors (same as map UI).
        z = barrier_path_diff_m(
            float(building_dist_m) if building_dist_m else 40.0,
            DEFAULT_BUILDING_H_M,
            A_DIV_REF_M,
            hr_m=hr,
        )
    ctx = {
        "t_c": 20.0 if t_c is None else t_c,
        "rh_pct": 70.0 if rh_pct is None else rh_pct,
        "p_kpa": p_kpa,
        "f_hz": f_hz,
        "forest_path_m": forest_path_m,
        "g": g,
        "barrier_z_m": z,
        "hr_m": hr,
        "hs_m": hs_m,
    }
    return float(envelope_at(bearing_deg, ctx)["r_m"])


def met_from_dict(m: dict[str, Any] | None) -> dict[str, Any]:
    if not m:
        return {}
    out: dict[str, Any] = {}
    for src, dst in (
        ("temperature_2m", "t_c"),
        ("relative_humidity_2m", "rh_pct"),
        ("surface_pressure", "p_hpa"),
        ("precipitation", "precip_mm"),
        ("wind_speed_10m", "wind_ms"),
        ("wind_direction_10m", "wind_from_deg"),
        ("wind_gusts_10m", "gust_ms"),
    ):
        if src in m and m[src] is not None:
            out[dst] = m[src]
    if "is_day" in m:
        out["is_day"] = m["is_day"]
    return out


__all__ = [
    "BASE_R_M",
    "barrier_db",
    "envelope_at",
    "foliage_atten_db",
    "foliage_scale",
    "ground_extra_db",
    "met_from_dict",
    "radius_at",
    "range_m",
]
