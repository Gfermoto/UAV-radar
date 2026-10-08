"""Ecowitt public share → Hub weather current{} (no second station upload)."""
from __future__ import annotations

import json
import math
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Callable

_LOCK = threading.Lock()
_CACHE: dict[str, tuple[float, dict[str, Any]]] = {}
_CACHE_TTL_S = 5 * 60.0
_TIMEOUT_S = 10.0
_UA = "NEVOD-Hub/0.1.62 (lab; ecowitt-share; +https://github.com/Gfermoto/nevod)"
_MMHG_TO_HPA = 1.33322387415

_URL_PROVIDER: Callable[[], str] | None = None
_RADIUS_PROVIDER: Callable[[], float] | None = None


def set_share_url_provider(fn: Callable[[], str] | None) -> None:
    global _URL_PROVIDER
    _URL_PROVIDER = fn


def set_radius_km_provider(fn: Callable[[], float] | None) -> None:
    global _RADIUS_PROVIDER
    _RADIUS_PROVIDER = fn


def share_url() -> str:
    if _URL_PROVIDER is not None:
        try:
            return str(_URL_PROVIDER() or "").strip()
        except Exception:
            return ""
    return ""


def radius_km() -> float:
    if _RADIUS_PROVIDER is not None:
        try:
            r = float(_RADIUS_PROVIDER())
            if 1.0 <= r <= 50.0:
                return r
        except Exception:
            pass
    return 8.0


def parse_share_url(url: str) -> tuple[str, str] | None:
    """Extract (authorize, device_id) from a home/share URL or bare query."""
    raw = (url or "").strip()
    if not raw:
        return None
    if "://" not in raw and "authorize=" in raw:
        raw = "https://www.ecowitt.net/home/share?" + raw.lstrip("?&")
    try:
        parts = urllib.parse.urlparse(raw)
    except Exception:
        return None
    qs = urllib.parse.parse_qs(parts.query)
    auth = (qs.get("authorize") or [""])[0].strip()
    did = (qs.get("device_id") or [""])[0].strip()
    if not auth or not did:
        return None
    if len(auth) < 4 or len(did) < 8:
        return None
    return auth, did


def haversine_km(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r = 6371.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlmb = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlmb / 2) ** 2
    return 2 * r * math.asin(min(1.0, math.sqrt(a)))


def _get_json(url: str) -> dict[str, Any]:
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


def _leaf(block: dict[str, Any] | None, key: str) -> Any:
    if not isinstance(block, dict):
        return None
    data = block.get("data")
    if not isinstance(data, dict):
        return None
    node = data.get(key)
    if not isinstance(node, dict):
        return None
    return node.get("value")


def _num(v: Any) -> float | None:
    if v is None:
        return None
    try:
        s = str(v).strip().replace(",", ".")
        if not s or s == "-":
            return None
        return float(s)
    except (TypeError, ValueError):
        return None


def normalize_home(home: dict[str, Any], info: dict[str, Any] | None = None) -> dict[str, Any]:
    """Map Ecowitt index/home payload to Open-Meteo-like current{}."""
    data = home.get("data") if isinstance(home.get("data"), dict) else {}
    temp_b = data.get("temp") if isinstance(data.get("temp"), dict) else {}
    wind_b = data.get("wind") if isinstance(data.get("wind"), dict) else {}
    rain_b = data.get("rain") if isinstance(data.get("rain"), dict) else {}
    press_b = data.get("pressure") if isinstance(data.get("pressure"), dict) else {}
    uv_b = data.get("so_uv") if isinstance(data.get("so_uv"), dict) else {}

    t = _num(_leaf(temp_b, "tempf"))
    rh = _num(_leaf(temp_b, "humidity"))
    wind = _num(_leaf(wind_b, "windspeedmph"))
    gust = _num(_leaf(wind_b, "windgustmph"))
    wdir = _num(_leaf(wind_b, "winddir"))
    precip = _num(_leaf(rain_b, "rainratein"))
    p_mmhg = _num(_leaf(press_b, "baromrelin"))
    p_hpa = round(p_mmhg * _MMHG_TO_HPA, 1) if p_mmhg is not None else None

    sunrise = _leaf(uv_b, "sunrise_time")
    sunset = _leaf(uv_b, "sunset_time")

    out: dict[str, Any] = {
        "current": {
            "temperature_2m": t,
            "relative_humidity_2m": rh,
            "surface_pressure": p_hpa,
            "wind_speed_10m": wind,
            "wind_direction_10m": wdir,
            "precipitation": precip if precip is not None else 0.0,
            "wind_gusts_10m": gust if gust is not None else wind,
            "is_day": None,
        },
        "daily": {
            "sunrise": [sunrise if isinstance(sunrise, str) else None],
            "sunset": [sunset if isinstance(sunset, str) else None],
        },
        "source": "ecowitt-share",
        "pressure_unit_in": "mmHg",
    }
    if info and info.get("latitude") is not None and info.get("longitude") is not None:
        try:
            lat = float(info.get("latitude"))  # type: ignore[arg-type]
            lon = float(info.get("longitude"))  # type: ignore[arg-type]
            out["station"] = {
                "name": str(info.get("name") or ""),
                "model": str(info.get("model") or ""),
                "latitude": lat,
                "longitude": lon,
            }
        except (TypeError, ValueError):
            pass
    return out


def fetch_share(authorize: str, device_id: str) -> dict[str, Any]:
    q = urllib.parse.urlencode({"authorize": authorize, "device_id": device_id})
    home = _get_json("https://www.ecowitt.net/index/home?" + q)
    if str(home.get("errcode", "0")) not in ("0",):
        raise ValueError(str(home.get("errmsg") or "ecowitt_home_fail"))
    info: dict[str, Any] | None = None
    try:
        info = _get_json("https://www.ecowitt.net/index/get_device_info?" + q)
    except Exception:
        info = None
    return normalize_home(home, info)


def cached_station_weather() -> dict[str, Any] | None:
    """Return normalized weather for configured share URL, or None."""
    url = share_url()
    parsed = parse_share_url(url)
    if not parsed:
        return None
    auth, did = parsed
    key = f"{auth}:{did}"
    hit: tuple[float, dict[str, Any]] | None
    with _LOCK:
        hit = _CACHE.get(key)
        if hit and time.time() - hit[0] < _CACHE_TTL_S:
            return hit[1]
    try:
        payload = fetch_share(auth, did)
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, ValueError, json.JSONDecodeError):
        return hit[1] if hit else None
    with _LOCK:
        _CACHE[key] = (time.time(), payload)
    return payload


def weather_if_near(lat: float, lon: float) -> dict[str, Any] | None:
    """If point is within radius of station, return ecowitt current payload."""
    wx = cached_station_weather()
    if not wx:
        return None
    st = wx.get("station") if isinstance(wx.get("station"), dict) else None
    if not st:
        return wx  # no coords → treat as site-local override when URL configured
    try:
        slat = float(st["latitude"])
        slon = float(st["longitude"])
    except (KeyError, TypeError, ValueError):
        return wx
    if haversine_km(lat, lon, slat, slon) <= radius_km():
        return wx
    return None


def clear_cache() -> None:
    with _LOCK:
        _CACHE.clear()
