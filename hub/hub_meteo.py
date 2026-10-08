"""Hub proxy for map weather (browser must not hit Open-Meteo directly)."""
from __future__ import annotations

import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

_LOCK = threading.Lock()
_CACHE: dict[str, tuple[float, dict[str, Any]]] = {}
_CACHE_TTL_S = 10 * 60.0
_ELEV_CACHE: dict[str, tuple[float, list[float | None]]] = {}
_ELEV_TTL_S = 6 * 3600.0
_TIMEOUT_S = 8.0
_UA = "NEVOD-Hub/0.1.69 (lab; map-wx; +https://github.com/Gfermoto/nevod)"
_KEY_PROVIDER = None  # optional Callable[[], str]


def clear_cache() -> None:
    """Drop weather (+ elev) caches after settings change. Public API — do not poke _CACHE."""
    with _LOCK:
        _CACHE.clear()
        _ELEV_CACHE.clear()


def set_api_key_provider(fn) -> None:
    """Wire Hub STORE meta lookup (or tests). Env OPEN_METEO_API_KEY remains fallback."""
    global _KEY_PROVIDER
    _KEY_PROVIDER = fn


def _api_key() -> str:
    if _KEY_PROVIDER is not None:
        try:
            k = str(_KEY_PROVIDER() or "").strip()
        except Exception:
            k = ""
        if k:
            return k
    return (os.environ.get("OPEN_METEO_API_KEY") or "").strip()


def _open_meteo_base() -> tuple[str, str]:
    """Return (base_url, apikey). Free or customer-api if key set (settings or env)."""
    key = _api_key()
    if key:
        return "https://customer-api.open-meteo.com", key
    return "https://api.open-meteo.com", ""


def _get(url: str) -> dict[str, Any]:
    req = urllib.request.Request(
        url,
        headers={"Accept": "application/json", "User-Agent": _UA},
    )
    with urllib.request.urlopen(req, timeout=_TIMEOUT_S) as resp:
        raw = resp.read()
    obj = json.loads(raw.decode("utf-8"))
    if not isinstance(obj, dict):
        raise ValueError("bad_json")
    return obj


def _from_open_meteo(lat: float, lon: float) -> dict[str, Any]:
    base, key = _open_meteo_base()
    params: dict[str, Any] = {
        "latitude": f"{lat:.4f}",
        "longitude": f"{lon:.4f}",
        "current": (
            "temperature_2m,relative_humidity_2m,surface_pressure,"
            "wind_speed_10m,wind_direction_10m,precipitation,wind_gusts_10m,is_day"
        ),
        "daily": "sunrise,sunset",
        "forecast_days": 1,
        "wind_speed_unit": "ms",
        "timezone": "auto",
    }
    if key:
        params["apikey"] = key
    j = _get(base + "/v1/forecast?" + urllib.parse.urlencode(params))
    j["source"] = "open-meteo" + ("-customer" if key else "")
    return j


def _from_metno(lat: float, lon: float) -> dict[str, Any]:
    q = urllib.parse.urlencode({"lat": f"{lat:.4f}", "lon": f"{lon:.4f}"})
    j = _get("https://api.met.no/weatherapi/locationforecast/2.0/compact?" + q)
    series = (((j.get("properties") or {}).get("timeseries")) or [])
    if not series:
        raise ValueError("metno_empty")
    instant = (((series[0].get("data") or {}).get("instant") or {}).get("details")) or {}
    nxt = (((series[0].get("data") or {}).get("next_1_hours") or {}).get("details")) or {}
    return {
        "current": {
            "temperature_2m": instant.get("air_temperature"),
            "relative_humidity_2m": instant.get("relative_humidity"),
            "surface_pressure": instant.get("air_pressure_at_sea_level"),
            "wind_speed_10m": instant.get("wind_speed"),
            "wind_direction_10m": instant.get("wind_from_direction"),
            "precipitation": nxt.get("precipitation_amount") or 0,
            "wind_gusts_10m": instant.get("wind_speed_of_gust") or instant.get("wind_speed"),
            "is_day": None,
        },
        "daily": {"sunrise": [None], "sunset": [None]},
        "source": "met.no",
    }


def _elevation(lat: float, lon: float, sample_m: float) -> list[float | None] | None:
    from hub_dem import fetch_elevations, ring_points

    ekey = f"{lat:.3f},{lon:.3f},{int(sample_m)}"
    with _LOCK:
        hit = _ELEV_CACHE.get(ekey)
        if hit and time.time() - hit[0] < _ELEV_TTL_S:
            return hit[1]

    pts = ring_points(lat, lon, sample_m, n=16)
    base, key = _open_meteo_base()
    params: dict[str, Any] = {
        "latitude": ",".join(f"{p[0]:.5f}" for p in pts),
        "longitude": ",".join(f"{p[1]:.5f}" for p in pts),
    }
    if key:
        params["apikey"] = key
    el: list[float | None] | None = None
    try:
        j = _get(base + "/v1/elevation?" + urllib.parse.urlencode(params))
        got = j.get("elevation")
        if isinstance(got, list) and got and got[0] is not None:
            el = got
    except Exception:
        el = None
    if el is None:
        dem = fetch_elevations(pts)
        if dem and dem[0] is not None:
            el = dem
    if el is not None:
        with _LOCK:
            _ELEV_CACHE[ekey] = (time.time(), el)
    return el


def _attach_elevation(payload: dict[str, Any], lat: float, lon: float, sample_m: float) -> None:
    el = _elevation(lat, lon, sample_m)
    if el:
        payload["elevation"] = el
        from hub_dem import isa_pressure_kpa

        cur = payload.get("current") or {}
        p_sea = cur.get("surface_pressure")
        if p_sea is not None and el[0] is not None:
            cur["pressure_local_kpa"] = round(isa_pressure_kpa(float(p_sea), float(el[0])), 3)
            payload["current"] = cur
            payload["pressure_uncorrected"] = False
        else:
            payload["pressure_uncorrected"] = True
    else:
        payload["pressure_uncorrected"] = True


def proxy_meteo(lat: float, lon: float, sample_m: float = 300.0) -> tuple[int, dict[str, Any]]:
    if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
        return 400, {"error": "bad_coord"}
    if lat == 0.0 and lon == 0.0:
        return 400, {"error": "bad_coord"}
    key = f"{lat:.3f},{lon:.3f},{int(sample_m)}"
    cached = _CACHE.get(key)
    if cached and time.time() - cached[0] < _CACHE_TTL_S:
        return 200, cached[1]

    payload: dict[str, Any] | None = None
    try:
        from hub_ecowitt_share import weather_if_near

        eco = weather_if_near(lat, lon)
        if eco and isinstance(eco.get("current"), dict):
            payload = dict(eco)
    except Exception:
        payload = None

    last_err = "upstream_fail"
    if payload is None:
        for fn in (_from_open_meteo, _from_metno):
            try:
                payload = fn(lat, lon)
                if isinstance(payload.get("current"), dict):
                    break
            except urllib.error.HTTPError as e:
                last_err = f"http_{e.code}"
                payload = None
            except Exception as e:  # noqa: BLE001
                last_err = type(e).__name__
                payload = None
    if payload is None:
        if cached:
            return 200, cached[1]
        return 502, {"error": last_err}

    _attach_elevation(payload, lat, lon, sample_m)
    with _LOCK:
        _CACHE[key] = (time.time(), payload)
    return 200, payload
