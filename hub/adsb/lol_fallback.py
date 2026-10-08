"""adsb.lol → readsb-like aircraft.json (Hub ADS-B reserve feed).

ODbL data: https://api.adsb.lol/docs
Only used when local feeder missing / disabled / blind (no positions).
"""
from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.request
from typing import Any
from urllib.parse import urlparse

from hub_webhooks import urlopen_no_redirect

ADS_B_LOL_HOST = "api.adsb.lol"
ADS_B_LOL_BASE = f"https://{ADS_B_LOL_HOST}/v2"
# nm; 200 ≈ 370 km — Московская область / SVO+VKO вокруг lab узлов.
DEFAULT_DIST_NM = 200.0
MAX_DIST_NM = 250.0
MAX_CENTERS = 3


def lol_fallback_enabled() -> bool:
    return os.environ.get("HUB_ADSB_LOL_FALLBACK", "1").strip() not in (
        "0",
        "false",
        "no",
        "off",
    )


def normalize_lol_to_aircraft_json(payload: dict[str, Any]) -> dict[str, Any]:
    """Map adsb.lol `{ac:[…]}` → dump1090/readsb `{aircraft:[…]}`."""
    raw_list = payload.get("ac")
    if not isinstance(raw_list, list):
        raw_list = payload.get("aircraft") if isinstance(payload.get("aircraft"), list) else []
    aircraft: list[dict[str, Any]] = []
    for item in raw_list:
        if not isinstance(item, dict):
            continue
        hx = item.get("hex")
        if not isinstance(hx, str) or not hx.strip():
            continue
        out = dict(item)
        out["hex"] = hx.strip().lower()
        # flight: prefer callsign; keep registration in r if present
        flight = out.get("flight")
        if isinstance(flight, str):
            out["flight"] = flight.strip()
        aircraft.append(out)
    now = payload.get("now")
    if not isinstance(now, (int, float)):
        now = time.time()
    return {
        "now": float(now),
        "messages": int(payload.get("messages") or 0),
        "aircraft": aircraft,
        "source": "adsb.lol",
    }


def _assert_lol_url(url: str) -> None:
    u = urlparse(url)
    if u.scheme != "https" or (u.hostname or "").lower() != ADS_B_LOL_HOST:
        raise ValueError("adsb_lol_host_not_allowed")
    if u.username or u.password:
        raise ValueError("adsb_lol_auth_forbidden")


def build_lol_url(lat: float, lon: float, dist_nm: float = DEFAULT_DIST_NM) -> str:
    d = max(1.0, min(float(dist_nm), MAX_DIST_NM))
    return f"{ADS_B_LOL_BASE}/lat/{float(lat):.5f}/lon/{float(lon):.5f}/dist/{d:.0f}"


def fetch_adsb_lol(
    lat: float,
    lon: float,
    *,
    dist_nm: float = DEFAULT_DIST_NM,
    timeout_s: float = 8.0,
) -> dict[str, Any]:
    """GET adsb.lol point API → normalized aircraft.json."""
    url = build_lol_url(lat, lon, dist_nm)
    _assert_lol_url(url)
    req = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "NEVOD-Hub/adsb-lol-fallback (+https://github.com/Gfermoto/nevod)",
        },
    )
    try:
        with urlopen_no_redirect(req, timeout=timeout_s) as resp:  # noqa: S310
            payload = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if 300 <= int(e.code) < 400:
            raise ValueError(f"adsb_lol_redirect_blocked:{e.code}") from e
        raise
    if not isinstance(payload, dict):
        raise ValueError("adsb_lol_bad_json")
    return normalize_lol_to_aircraft_json(payload)


def fetch_adsb_lol_multi(
    centers: list[tuple[float, float]],
    *,
    dist_nm: float = DEFAULT_DIST_NM,
    timeout_s: float = 8.0,
) -> dict[str, Any]:
    """Несколько точек (зона + узлы) → merge по hex (покрытие шире одного центра)."""
    uniq: list[tuple[float, float]] = []
    seen: set[tuple[float, float]] = set()
    for lat, lon in centers:
        key = (round(float(lat), 2), round(float(lon), 2))
        if key in seen:
            continue
        seen.add(key)
        uniq.append((float(lat), float(lon)))
        if len(uniq) >= MAX_CENTERS:
            break
    if not uniq:
        raise ValueError("adsb_lol_no_centers")
    by_hex: dict[str, dict[str, Any]] = {}
    last_url = ""
    errors: list[str] = []
    for i, (lat, lon) in enumerate(uniq):
        if i:
            time.sleep(1.05)  # rate-limit adsb.lol
        try:
            part = fetch_adsb_lol(lat, lon, dist_nm=dist_nm, timeout_s=timeout_s)
            last_url = build_lol_url(lat, lon, dist_nm)
            for ac in part.get("aircraft") or []:
                if not isinstance(ac, dict):
                    continue
                hx = str(ac.get("hex") or "").lower()
                if hx:
                    by_hex[hx] = ac
        except Exception as e:  # noqa: BLE001
            errors.append(f"{lat:.3f},{lon:.3f}:{type(e).__name__}")
    if not by_hex and errors:
        raise ValueError("adsb_lol_multi_fail:" + ";".join(errors[:3]))
    return {
        "now": time.time(),
        "messages": 0,
        "aircraft": list(by_hex.values()),
        "source": "adsb.lol",
        "centers": len(uniq),
        "url": last_url,
        "errors": errors,
    }


def is_blind_aircraft_json(raw: dict[str, Any]) -> bool:
    """True if feed answered but no usable positions (ослеп)."""
    aircraft = raw.get("aircraft") if isinstance(raw.get("aircraft"), list) else []
    if not aircraft:
        return True
    for item in aircraft:
        if not isinstance(item, dict):
            continue
        lat, lon = item.get("lat"), item.get("lon")
        if isinstance(lat, (int, float)) and isinstance(lon, (int, float)):
            return False
    return True
