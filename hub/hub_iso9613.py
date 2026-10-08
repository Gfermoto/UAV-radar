"""ISO 9613 outdoor attenuation for Hub map envelope (no HTTP)."""
from __future__ import annotations

import math
from typing import Any

T0 = 293.15
T01 = 273.16
P_REF = 101.325
A_DIV_REF_M = 500.0
F_REF_HZ = 500.0
DRAW_MIN_M = 50.0
DRAW_MAX_M = 2000.0
HS_M = 30.0
HR_M = 1.5
# OSM building without height/levels — suburban 2 floors (partial).
DEFAULT_BUILDING_H_M = 6.0

# ISO 9613-2:1996 Annex A table A.1 — (dB for 10–20 m, dB/m for 20–200 m)
_FOLIAGE: dict[int, tuple[float, float]] = {
    63: (0.0, 0.02),
    125: (0.0, 0.03),
    250: (1.0, 0.04),
    500: (1.0, 0.05),
    1000: (1.0, 0.06),
    2000: (1.0, 0.08),
    4000: (2.0, 0.09),
    8000: (3.0, 0.12),
}
_OCTAVES = tuple(_FOLIAGE.keys())


def alpha_db_per_m(f_hz: float, t_c: float, rh_pct: float, p_kpa: float) -> float:
    """ISO 9613-1:1993 §4 / Bass 1995 atmospheric absorption α, dB/m."""
    f = float(f_hz)
    t = float(t_c) + 273.15
    p = float(p_kpa)
    if f <= 0 or p <= 0 or t <= 0:
        return 0.0
    psat_over_pref = 10 ** (-6.8346 * (T01 / t) ** 1.261 + 4.6151)
    h = float(rh_pct) * psat_over_pref / (p / P_REF)
    pr = p / P_REF
    fro = pr * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h))
    frn = (
        pr
        * (t / T0) ** -0.5
        * (9.0 + 280.0 * h * math.exp(-4.170 * ((t / T0) ** (-1.0 / 3.0) - 1.0)))
    )
    a_class = 1.84e-11 * (P_REF / p) * math.sqrt(t / T0)
    a_o = 0.01275 * math.exp(-2239.1 / t) * fro / (fro * fro + f * f)
    a_n = 0.1068 * math.exp(-3352.0 / t) * frn / (frn * frn + f * f)
    return 8.686 * f * f * (a_class + (t / T0) ** -2.5 * (a_o + a_n))


def nearest_octave_hz(f_hz: float) -> int:
    f = float(f_hz)
    return min(_OCTAVES, key=lambda o: abs(o - f))


def adiv_db(d_m: float) -> float:
    """ISO 9613-2 eq. (7) geometrical divergence."""
    d = max(1.0, float(d_m))
    return 20.0 * math.log10(d) + 11.0


def range_m(
    extra_db: float,
    *,
    f_hz: float,
    t_c: float,
    rh_pct: float,
    p_kpa: float,
) -> float:
    """Distance where Adiv+Aatm equals reference budget minus extra_db."""
    f = float(f_hz)
    aref = adiv_db(A_DIV_REF_M) + alpha_db_per_m(f, 20.0, 70.0, 101.325) * A_DIV_REF_M
    target = aref - float(extra_db)
    lo, hi = DRAW_MIN_M, DRAW_MAX_M
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        a = adiv_db(mid) + alpha_db_per_m(f, t_c, rh_pct, p_kpa) * mid
        if a < target:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def foliage_db(path_m: float, f_hz: float) -> float:
    """ISO 9613-2 Annex A foliage attenuation (nearest octave)."""
    path = float(path_m)
    if path < 10.0:
        return 0.0
    band = nearest_octave_hz(f_hz)
    short_db, rate = _FOLIAGE[band]
    if path <= 20.0:
        return float(short_db)
    capped = min(path, 200.0)
    return float(rate) * capped


def agr_porous_db(d_m: float, hs_m: float = HS_M, hr_m: float = HR_M) -> float:
    """ISO 9613-2 eq. (10) porous-ground alternative method."""
    d = max(1.0, float(d_m))
    hm = (float(hs_m) + float(hr_m)) / 2.0
    agr = 4.8 - (2.0 * hm / d) * (17.0 + 300.0 / d)
    return 0.0 if agr < 0.0 else agr


def ground_extra_db(
    g: float, d_m: float, *, hs_m: float = HS_M, hr_m: float = HR_M
) -> float:
    """Contribution to extra_db vs porous reference (G=1)."""
    g = max(0.0, min(1.0, float(g)))
    agr = agr_porous_db(d_m, hs_m=hs_m, hr_m=hr_m)
    return (g - 1.0) * agr


def barrier_path_diff_m(
    d_barrier: float,
    height_m: float,
    range_m: float = A_DIV_REF_M,
    *,
    hs_m: float = HS_M,
    hr_m: float = HR_M,
) -> float:
    """ISO 9613-2 path-length difference δ for a thin screen (metres)."""
    r = max(50.0, float(range_m))
    h = max(0.0, float(height_m))
    dsr = max(1.0, min(float(d_barrier), r - 1.0))
    dss = max(1.0, r - dsr)
    via = math.hypot(dss, float(hs_m) - h) + math.hypot(dsr, h - float(hr_m))
    return max(0.0, via - r)


def barrier_dz_db(z_m: float, f_hz: float) -> float:
    """ISO 9613-2 eq. (14) single thin edge, C2=20, C3=1, Kmet=1; Dz≤20."""
    if z_m <= 0 or f_hz <= 0:
        return 0.0
    lam = 340.0 / float(f_hz)
    dz = 10.0 * math.log10(3.0 + (20.0 / lam) * float(z_m))
    if dz < 0:
        return 0.0
    return min(20.0, dz)


def barrier_db(
    z_m: float, f_hz: float, *, dz_only: bool = False, hs_m: float = HS_M, hr_m: float = HR_M
) -> float:
    dz = barrier_dz_db(z_m, f_hz)
    if dz_only:
        return dz
    if dz <= 0:
        return 0.0
    # eq. (12): Abar = max(Dz − Agr, 0); Agr taken at reference distance
    return max(0.0, dz - agr_porous_db(A_DIV_REF_M, hs_m=hs_m, hr_m=hr_m))


def envelope_at(bearing_deg: float, ctx: dict[str, Any]) -> dict[str, Any]:
    """Assemble ray envelope. bearing_deg kept for API symmetry with the map."""
    _ = bearing_deg
    f_hz = float(ctx.get("f_hz") or F_REF_HZ)
    t_c = float(ctx.get("t_c") if ctx.get("t_c") is not None else 20.0)
    rh_pct = float(ctx.get("rh_pct") if ctx.get("rh_pct") is not None else 70.0)
    p_kpa = float(ctx.get("p_kpa") if ctx.get("p_kpa") is not None else 101.325)
    forest = float(ctx.get("forest_path_m") or 0.0)
    g = float(ctx.get("g") if ctx.get("g") is not None else 1.0)
    z = float(ctx.get("barrier_z_m") or 0.0)
    hs_m = float(ctx.get("hs_m") if ctx.get("hs_m") is not None else HS_M)
    hr_m = float(ctx.get("hr_m") if ctx.get("hr_m") is not None else HR_M)
    if hr_m < 0.3:
        hr_m = HR_M
    if hr_m > 80.0:
        hr_m = 80.0

    a_fol = foliage_db(forest, f_hz)
    a_bar = barrier_db(z, f_hz, hs_m=hs_m, hr_m=hr_m)
    if a_bar > 0:
        a_gr_extra = 0.0
    else:
        a_gr_extra = ground_extra_db(g, A_DIV_REF_M, hs_m=hs_m, hr_m=hr_m)
    extra = a_gr_extra + a_fol + a_bar
    r = range_m(extra, f_hz=f_hz, t_c=t_c, rh_pct=rh_pct, p_kpa=p_kpa)
    clamped = r <= DRAW_MIN_M + 0.5 or r >= DRAW_MAX_M - 0.5
    a_atm = alpha_db_per_m(f_hz, t_c, rh_pct, p_kpa) * r
    return {
        "r_m": r,
        "f_hz": f_hz,
        "a_div_db": adiv_db(r),
        "a_atm_db": a_atm,
        "a_gr_db": a_gr_extra,
        "a_bar_db": a_bar,
        "a_fol_db": a_fol,
        "clamped": clamped,
        "extra_db": extra,
        "hr_m": hr_m,
        "hs_m": hs_m,
    }
