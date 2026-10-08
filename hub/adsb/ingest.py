"""Hub ADS-B ingest (#114): HTTP aircraft.json → MQTT health + per-hex.

Lab SoT URL (default):
  http://192.168.1.117/skyaware/data/aircraft.json

Usage:
  python3 iot/tools/adsb/ingest.py --dry-run --once
  python3 iot/tools/adsb/ingest.py --mqtt-host 192.168.1.10 --site lab
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from hub_webhooks import urlopen_no_redirect, webhook_url_allowed  # noqa: E402
from adsb.schema import (  # noqa: E402
    AC_STALE_S,
    POLL_INTERVAL_S_DEFAULT,
    RETAIN_AC,
    RETAIN_HEALTH,
    RETAIN_SNAPSHOT,
    QOS_AC,
    QOS_HEALTH,
    QOS_SNAPSHOT,
    build_ac,
    build_health,
    normalize_hex,
    topic_ac,
    topic_health,
    topic_snapshot,
)

LAB_ADSB_URL = "http://192.168.1.117/skyaware/data/aircraft.json"  # lab PiAware; was NAT :9999


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlmb = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlmb / 2) ** 2
    return 2 * r * math.asin(min(1.0, math.sqrt(a)))


def _adsb_allow_private() -> bool:
    """LAN feeder — default allow RFC1918; override HUB_ADSB_ALLOW_PRIVATE=0."""
    v = os.environ.get("HUB_ADSB_ALLOW_PRIVATE", "1").strip().lower()
    return v in ("1", "true", "yes", "on")


def assert_adsb_url_allowed(url: str) -> None:
    ok, reason = webhook_url_allowed(url, allow_private=_adsb_allow_private())
    if not ok:
        raise ValueError(f"adsb_ssrf:{reason}")


def fetch_aircraft_json(url: str, timeout_s: float = 5.0) -> dict[str, Any]:
    assert_adsb_url_allowed(url)
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    try:
        with urlopen_no_redirect(req, timeout=timeout_s) as resp:  # noqa: S310 lab
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if 300 <= int(e.code) < 400:
            raise ValueError(f"adsb_redirect_blocked:{e.code}") from e
        raise


def process_poll(
    raw: dict[str, Any],
    *,
    site_id: str,
    source_url: str,
    poll_age_s: float,
    site_lat: float | None,
    site_lon: float | None,
    radius_m: float,
    ts: str | None = None,
) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, Any]]:
    """Return (health, ac_list, meta) from one aircraft.json body."""
    ts = ts or _now_iso()
    aircraft = raw.get("aircraft") if isinstance(raw.get("aircraft"), list) else []
    with_pos = 0
    in_radius = 0
    nearest: float | None = None
    pubs: list[dict[str, Any]] = []
    for item in aircraft:
        if not isinstance(item, dict):
            continue
        try:
            hx = normalize_hex(item.get("hex"))
        except ValueError:
            continue
        lat, lon = item.get("lat"), item.get("lon")
        dist = None
        inside = None
        if isinstance(lat, (int, float)) and isinstance(lon, (int, float)):
            with_pos += 1
            if site_lat is not None and site_lon is not None:
                dist = haversine_m(float(site_lat), float(site_lon), float(lat), float(lon))
                inside = dist <= radius_m
                if inside:
                    in_radius += 1
                if nearest is None or dist < nearest:
                    nearest = dist
        pubs.append(
            build_ac(
                site_id=site_id,
                ts=ts,
                aircraft=item,
                dist_m=dist,
                in_radius=inside,
            )
        )
    health = build_health(
        site_id=site_id,
        ts=ts,
        total_aircraft=len(pubs),
        aircraft_with_positions=with_pos,
        in_radius_count=in_radius,
        adsb_radius_m=radius_m,
        source_url=source_url,
        poll_age_s=poll_age_s,
        ok=True,
        problem=None,
        decoder_now=raw.get("now") if isinstance(raw.get("now"), (int, float)) else None,
        decoder_messages=int(raw["messages"])
        if isinstance(raw.get("messages"), (int, float))
        else None,
    )
    meta = {
        "civilian_in_radius": in_radius > 0,
        "adsb_radius_m": radius_m,
        "adsb_count_in_radius": in_radius,
        "adsb_nearest_dist_m": nearest,
    }
    return health, pubs, meta


class MqttPublisher:
    def __init__(self, host: str, port: int, username: str, password: str) -> None:
        import paho.mqtt.client as mqtt  # type: ignore

        self._mqtt = mqtt
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        if username:
            self.client.username_pw_set(username, password or None)
        self.client.connect(host, port, keepalive=30)
        self.client.loop_start()

    def publish(self, topic: str, payload: dict[str, Any], *, qos: int, retain: bool) -> None:
        body = json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
        self.client.publish(topic, body, qos=qos, retain=retain)

    def close(self) -> None:
        try:
            self.client.loop_stop()
            self.client.disconnect()
        except Exception:
            pass


def run_loop(args: argparse.Namespace) -> int:
    pub = None
    if not args.dry_run:
        if not args.mqtt_host:
            print("need --mqtt-host or --dry-run", file=sys.stderr)
            return 2
        pub = MqttPublisher(args.mqtt_host, args.mqtt_port, args.mqtt_user, args.mqtt_pass)

    seen: dict[str, float] = {}
    try:
        n = 0
        while True:
            t0 = time.time()
            try:
                raw = fetch_aircraft_json(args.url, timeout_s=args.timeout)
                age = time.time() - t0
                health, acs, meta = process_poll(
                    raw,
                    site_id=args.site,
                    source_url=args.url,
                    poll_age_s=age,
                    site_lat=args.site_lat,
                    site_lon=args.site_lon,
                    radius_m=args.radius_m,
                )
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, ValueError) as e:
                age = time.time() - t0
                health = build_health(
                    site_id=args.site,
                    ts=_now_iso(),
                    total_aircraft=0,
                    aircraft_with_positions=0,
                    in_radius_count=0,
                    adsb_radius_m=args.radius_m,
                    source_url=args.url,
                    poll_age_s=age,
                    ok=False,
                    problem=type(e).__name__,
                )
                acs, meta = [], {"civilian_in_radius": False, "error": str(e)}

            th = topic_health(args.site)
            if args.dry_run:
                print("HEALTH", th, json.dumps(health, ensure_ascii=False)[:200])
            else:
                assert pub is not None
                pub.publish(th, health, qos=QOS_HEALTH, retain=RETAIN_HEALTH)

            now = time.time()
            for ac in acs:
                hx = ac["hex"]
                seen[hx] = now
                topic = topic_ac(args.site, hx)
                if args.dry_run:
                    print("AC", topic, f"in_radius={ac.get('in_radius')} dist={ac.get('dist_m')}")
                else:
                    assert pub is not None
                    pub.publish(topic, ac, qos=QOS_AC, retain=RETAIN_AC)

            # prune memory
            for hx in list(seen):
                if now - seen[hx] > AC_STALE_S:
                    del seen[hx]

            if args.snapshot and acs:
                snap = {
                    "schema": "nevod.adsb.snapshot.v1",
                    "site_id": args.site,
                    "ts": health.get("ts"),
                    "count": len(acs),
                    "aircraft": acs,
                }
                ts_topic = topic_snapshot(args.site)
                if args.dry_run:
                    print("SNAP", ts_topic, "count", len(acs))
                else:
                    assert pub is not None
                    pub.publish(ts_topic, snap, qos=QOS_SNAPSHOT, retain=RETAIN_SNAPSHOT)

            print(
                f"[adsb] ok={health.get('ok')} total={health.get('total_aircraft')} "
                f"pos={health.get('aircraft_with_positions')} "
                f"in_r={health.get('in_radius_count')} meta={meta}",
                flush=True,
            )
            n += 1
            if args.once or (args.max_polls and n >= args.max_polls):
                break
            time.sleep(max(0.2, args.interval))
    finally:
        if pub is not None:
            pub.close()
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Hub ADS-B ingest (#114)")
    ap.add_argument("--url", default=LAB_ADSB_URL)
    ap.add_argument("--site", default="lab")
    ap.add_argument("--interval", type=float, default=POLL_INTERVAL_S_DEFAULT)
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--radius-m", type=float, default=50000.0)
    ap.add_argument("--site-lat", type=float, default=None)
    ap.add_argument("--site-lon", type=float, default=None)
    ap.add_argument("--mqtt-host", default="")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--mqtt-user", default="")
    ap.add_argument("--mqtt-pass", default="")
    ap.add_argument("--snapshot", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--max-polls", type=int, default=0)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return _self_test()
    return run_loop(args)


def _self_test() -> int:
    raw = {
        "now": 1.0,
        "messages": 9,
        "aircraft": [
            {"hex": "abc123", "lat": 55.75, "lon": 37.62, "flight": "TST1"},
            {"hex": "def456", "flight": "NOPOS"},
        ],
    }
    health, acs, meta = process_poll(
        raw,
        site_id="lab",
        source_url="http://test/aircraft.json",
        poll_age_s=0.1,
        site_lat=55.75,
        site_lon=37.61,
        radius_m=50000,
        ts="2026-08-12T13:00:00Z",
    )
    assert health["ok"] and health["total_aircraft"] == 2
    assert health["aircraft_with_positions"] == 1
    assert health["in_radius_count"] == 1
    assert meta["civilian_in_radius"] is True
    assert any(a["hex"] == "ABC123" and a["in_radius"] for a in acs)
    assert any(a["hex"] == "DEF456" and a.get("in_radius") is None for a in acs)
    d = haversine_m(0, 0, 0, 1)
    assert 110000 < d < 120000  # ~111.3 km per deg lon at equator
    print("adsb ingest self-test OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
