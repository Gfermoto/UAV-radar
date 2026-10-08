"""FOG → Hub lab bridge: JSON /api/ingest + GET /api/v1/detections (#184).

Lab «Cloud» = Hub. FOG forwarder posts nevod.*.v1 JSON; cloud_pull reads detections.
Does not replace DIY Protobuf path (/api/v1/pb/*).
"""
from __future__ import annotations

import time
from typing import Any, Callable

FOG_INGEST_SCHEMAS = frozenset(
    {
        "nevod.track.v1",
        "nevod.detection.v1",
        "nevod.heartbeat.v1",
        "nevod.alert.v1",
    }
)


def validate_fog_ingest(msg: object) -> tuple[str | None, str | None]:
    """Return (schema, None) or (None, error)."""
    if not isinstance(msg, dict):
        return None, "bad_json_structure"
    schema = msg.get("schema")
    if not isinstance(schema, str) or schema not in FOG_INGEST_SCHEMAS:
        return None, "unknown_schema"
    if schema != "nevod.track.v1":
        nid = msg.get("node_id")
        if not isinstance(nid, str) or not nid.strip():
            return None, "missing_node_id"
    return schema, None


def _ts_ms(msg: dict[str, Any]) -> int:
    for k in ("ts_ms", "timestamp_ms", "last_seen", "first_seen"):
        v = msg.get(k)
        if isinstance(v, (int, float)) and v > 0:
            return int(v)
        if isinstance(v, str) and v.isdigit():
            return int(v)
    return int(time.time() * 1000)


def apply_fog_ingest(
    mark: Callable[..., None],
    msg: dict[str, Any],
    *,
    body_len: int,
) -> dict[str, Any]:
    """Map FOG JSON → HubState.mark keys. ``mark`` = HubState.mark bound method."""
    schema, err = validate_fog_ingest(msg)
    if err or not schema:
        raise ValueError(err or "invalid")

    if schema == "nevod.track.v1":
        meta = {
            "node_id": str(msg.get("node_id") or msg.get("track_id") or "fog-track")[:32],
            "track_id": msg.get("track_id"),
            "state": msg.get("state"),
            "threat": msg.get("threat"),
            "p": msg.get("p"),
            "class": (msg.get("target") or {}).get("class") or msg.get("class"),
            "n_nodes": msg.get("n_nodes"),
            "estimate": msg.get("estimate"),
            "schema": schema,
            "ts_ms": _ts_ms(msg),
            "source": "fog_forwarder",
        }
        mark("fog_track", ok=True, body_len=body_len, meta=meta, transport="fog_https")
        return {"accepted": True, "schema": schema, "track_id": msg.get("track_id")}

    if schema == "nevod.alert.v1":
        meta = {
            "node_id": str(msg.get("node_id") or "fog-alert")[:32],
            "threat": msg.get("threat"),
            "track_id": msg.get("track_id"),
            "schema": schema,
            "ts_ms": _ts_ms(msg),
            "source": "fog_forwarder",
        }
        mark("fog_alert", ok=True, body_len=body_len, meta=meta, transport="fog_https")
        return {"accepted": True, "schema": schema}

    if schema == "nevod.detection.v1":
        det = msg.get("detection") if isinstance(msg.get("detection"), dict) else {}
        meta = {
            "node_id": str(msg["node_id"]).strip()[:32],
            "threat": det.get("threat", msg.get("threat")),
            "confidence": det.get("p", msg.get("p")),
            "p": det.get("p", msg.get("p")),
            "class": det.get("class", msg.get("class")),
            "schema": schema,
            "ts_ms": _ts_ms(msg),
            "source": "fog_forwarder",
        }
        # Reuse DIY detection join path
        mark("pb_detection", ok=True, body_len=body_len, meta=meta, transport="fog_https")
        return {"accepted": True, "schema": schema}

    # heartbeat
    meta = {
        "node_id": str(msg["node_id"]).strip()[:32],
        "status": msg.get("status", "ok"),
        "fw": msg.get("fw"),
        "schema": schema,
        "ts_ms": _ts_ms(msg),
        "source": "fog_forwarder",
    }
    mark("pb_heartbeat", ok=True, body_len=body_len, meta=meta, transport="fog_https")
    return {"accepted": True, "schema": schema}


def list_detections_for_pull(
    nodes: dict[str, Any],
    *,
    since_ms: int = 0,
    limit: int = 100,
) -> list[dict[str, Any]]:
    """Build cloud_pull payload from Hub node last_detection."""
    out: list[dict[str, Any]] = []
    limit = max(1, min(int(limit), 500))
    since_ms = max(0, int(since_ms))
    for nid, node in nodes.items():
        det = getattr(node, "last_detection", None)
        if not isinstance(det, dict):
            continue
        ts = 0
        for k in ("ts_ms", "timestamp_ms"):
            v = det.get(k)
            if isinstance(v, (int, float)):
                ts = int(v)
                break
        if ts and ts < since_ms:
            continue
        out.append(
            {
                "schema": "nevod.detection.v1",
                "node_id": nid,
                "ts_ms": ts or int(time.time() * 1000),
                "detection": {
                    "class": det.get("class") or det.get("label"),
                    "p": det.get("p") or det.get("confidence"),
                    "threat": det.get("threat"),
                },
                "extensions": {
                    "via": det.get("via"),
                    "source_hint": det.get("source") or "hub",
                },
            }
        )
        if len(out) >= limit:
            break
    return out
