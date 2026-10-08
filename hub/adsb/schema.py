"""MQTT schema SoT for Hub ADS-B bridge (#113).

Topics (site_id = Hub site slug, e.g. ``lab``):

* ``nevod/{site_id}/adsb/health`` — aggregates each poll
* ``nevod/{site_id}/adsb/ac/{HEX}`` — one aircraft (HEX upper)
* ``nevod/{site_id}/adsb/aircraft`` — optional full snapshot (debug)

Not IQTLabs topic layout. Publisher = Hub ``adsb-ingest`` (#114).
"""

from __future__ import annotations

import re
from typing import Any

SCHEMA_HEALTH = "nevod.adsb.health.v1"
SCHEMA_AC = "nevod.adsb.ac.v1"
SCHEMA_SNAPSHOT = "nevod.adsb.snapshot.v1"

TOPIC_HEALTH = "nevod/{site_id}/adsb/health"
TOPIC_AC = "nevod/{site_id}/adsb/ac/{hex}"
TOPIC_SNAPSHOT = "nevod/{site_id}/adsb/aircraft"

# QoS / retain guidance for #114
QOS_HEALTH = 0
RETAIN_HEALTH = True  # last known health for HA
QOS_AC = 0
RETAIN_AC = False  # avoid stale hex in broker
QOS_SNAPSHOT = 0
RETAIN_SNAPSHOT = False

# Poll / publish guidance
POLL_INTERVAL_S_DEFAULT = 2.0
POLL_INTERVAL_S_MIN = 1.0
POLL_INTERVAL_S_MAX = 10.0
HEALTH_EVERY_POLL = True
# Re-publish per-hex at least every N polls even if unchanged (HA freshness)
AC_REFRESH_POLLS = 15
# Drop hex from local cache (and stop publishing) after unseen this long
AC_STALE_S = 60.0

_HEX_RE = re.compile(r"^[0-9A-Fa-f]{6}$")
_SITE_RE = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9_-]{0,31}$")


def normalize_hex(raw: Any) -> str:
    if not isinstance(raw, str):
        raise ValueError("hex_not_str")
    h = raw.strip().upper()
    if h.startswith("~"):  # non-ICAO / anonymous marker in some feeds
        h = h[1:]
    if not _HEX_RE.match(h):
        raise ValueError(f"bad_hex:{raw!r}")
    return h


def topic_health(site_id: str) -> str:
    _require_site(site_id)
    return TOPIC_HEALTH.format(site_id=site_id)


def topic_ac(site_id: str, hex_id: str) -> str:
    _require_site(site_id)
    return TOPIC_AC.format(site_id=site_id, hex=normalize_hex(hex_id))


def topic_snapshot(site_id: str) -> str:
    _require_site(site_id)
    return TOPIC_SNAPSHOT.format(site_id=site_id)


def _require_site(site_id: str) -> None:
    if not isinstance(site_id, str) or not _SITE_RE.match(site_id):
        raise ValueError(f"bad_site_id:{site_id!r}")


def validate_health(msg: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(msg, dict):
        raise ValueError("health_not_object")
    if msg.get("schema") != SCHEMA_HEALTH:
        raise ValueError("bad_schema_health")
    _require_site(str(msg.get("site_id") or ""))
    if not isinstance(msg.get("ts"), str) or not msg["ts"].strip():
        raise ValueError("missing_ts")
    ok = msg.get("ok")
    if not isinstance(ok, bool):
        raise ValueError("ok_not_bool")
    for k in ("total_aircraft", "aircraft_with_positions", "in_radius_count"):
        v = msg.get(k)
        if not isinstance(v, int) or v < 0:
            raise ValueError(f"bad_{k}")
    if msg.get("adsb_radius_m") is not None:
        r = float(msg["adsb_radius_m"])
        if r <= 0:
            raise ValueError("bad_adsb_radius_m")
    if not ok and not msg.get("problem"):
        raise ValueError("problem_required_when_not_ok")
    return msg


def validate_ac(msg: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(msg, dict):
        raise ValueError("ac_not_object")
    if msg.get("schema") != SCHEMA_AC:
        raise ValueError("bad_schema_ac")
    _require_site(str(msg.get("site_id") or ""))
    hx = normalize_hex(msg.get("hex"))
    if msg.get("hex") != hx:
        raise ValueError("hex_not_normalized")
    ac = msg.get("aircraft")
    if not isinstance(ac, dict) or not ac:
        raise ValueError("aircraft_missing")
    # Passthrough: require matching hex (case-insensitive)
    src_hex = str(ac.get("hex") or "").strip().lstrip("~")
    if src_hex.upper() != hx:
        raise ValueError("aircraft_hex_mismatch")
    if msg.get("dist_m") is not None:
        float(msg["dist_m"])
    if msg.get("in_radius") is not None and not isinstance(msg["in_radius"], bool):
        raise ValueError("in_radius_not_bool")
    return msg


def validate_snapshot(msg: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(msg, dict):
        raise ValueError("snapshot_not_object")
    if msg.get("schema") != SCHEMA_SNAPSHOT:
        raise ValueError("bad_schema_snapshot")
    _require_site(str(msg.get("site_id") or ""))
    rows = msg.get("aircraft")
    if not isinstance(rows, list):
        raise ValueError("aircraft_not_list")
    for row in rows:
        validate_ac(row)
    if msg.get("count") is not None and int(msg["count"]) != len(rows):
        raise ValueError("count_mismatch")
    return msg


def build_health(
    *,
    site_id: str,
    ts: str,
    total_aircraft: int,
    aircraft_with_positions: int,
    in_radius_count: int,
    adsb_radius_m: float,
    source_url: str,
    poll_age_s: float,
    ok: bool = True,
    problem: str | None = None,
    decoder_now: float | None = None,
    decoder_messages: int | None = None,
) -> dict[str, Any]:
    out: dict[str, Any] = {
        "schema": SCHEMA_HEALTH,
        "site_id": site_id,
        "ts": ts,
        "ok": ok,
        "problem": problem,
        "source_url": source_url,
        "poll_age_s": poll_age_s,
        "total_aircraft": int(total_aircraft),
        "aircraft_with_positions": int(aircraft_with_positions),
        "in_radius_count": int(in_radius_count),
        "adsb_radius_m": float(adsb_radius_m),
    }
    if decoder_now is not None:
        out["decoder_now"] = float(decoder_now)
    if decoder_messages is not None:
        out["decoder_messages"] = int(decoder_messages)
    return validate_health(out)


def build_ac(
    *,
    site_id: str,
    ts: str,
    aircraft: dict[str, Any],
    dist_m: float | None = None,
    in_radius: bool | None = None,
    source: str = "aircraft.json",
) -> dict[str, Any]:
    hx = normalize_hex(aircraft.get("hex"))
    out: dict[str, Any] = {
        "schema": SCHEMA_AC,
        "site_id": site_id,
        "ts": ts,
        "source": source,
        "hex": hx,
        "aircraft": dict(aircraft),
    }
    if dist_m is not None:
        out["dist_m"] = float(dist_m)
    if in_radius is not None:
        out["in_radius"] = bool(in_radius)
    return validate_ac(out)
