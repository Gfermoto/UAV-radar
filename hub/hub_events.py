#!/usr/bin/env python3
"""Ingest timeline event summary / enrich for Nevod Hub UI."""
from __future__ import annotations

from typing import Any

VIA_LABELS = {
    "pb_detection": "PB · DET",
    "pb_heartbeat": "PB · HB",
    "pb_mel": "PB · MEL",
    "mqtt_detection": "MQTT · DET",
    "mqtt_heartbeat": "MQTT · HB",
    "episode": "Эпизод",
}

def _event_transport_kind(key: str | None) -> tuple[str, str]:
    k = (key or "").strip()
    if k == "episode":
        # transport берём из meta.via в enrich_event
        return "", "det"
    if k.startswith("mqtt_"):
        transport = "mqtt"
        kind = {"mqtt_detection": "det", "mqtt_heartbeat": "hb"}.get(k, "")
    elif k.startswith("pb_"):
        transport = "pb"
        kind = {
            "pb_detection": "det",
            "pb_heartbeat": "hb",
            "pb_mel": "mel",
        }.get(k, "")
    else:
        transport, kind = "", ""
    return transport, kind


def _fmt_heap_short(v: Any) -> str | None:
    try:
        n = float(v)
    except (TypeError, ValueError):
        return None
    if n >= 1024:
        return f"{n/1024.0:.1f} KB"
    return f"{int(n)} B"


def build_event_summary(key: str | None, meta: dict[str, Any] | None, *, ok: bool = True, error: str = "") -> str:
    meta = meta or {}
    if not ok and error:
        return f"✗ {error}"
    transport, kind = _event_transport_kind(key)
    bits: list[str] = []
    if kind == "det":
        if meta.get("hop_count") is not None:
            bits.append(f"{int(meta['hop_count'])} hops")
        if meta.get("mel_count"):
            try:
                bits.append(f"{int(meta['mel_count'])} mel")
            except (TypeError, ValueError):
                pass
        if meta.get("open") is False:
            bits.append("закрыт")
        elif meta.get("open") is True and meta.get("episode_id"):
            bits.append("активен")
        if meta.get("class_name"):
            bits.append(str(meta["class_name"]))
        seen = meta.get("classes_seen")
        if isinstance(seen, list) and len(seen) > 1:
            bits.append("→".join(str(x) for x in seen))
        if meta.get("threat") is not None:
            try:
                bits.append(f"threat {float(meta['threat']):.2f}")
            except (TypeError, ValueError):
                bits.append(f"threat {meta['threat']}")
        if meta.get("confidence") is not None or meta.get("p") is not None:
            p = meta.get("confidence", meta.get("p"))
            try:
                bits.append(f"p {float(p):.2f}")
            except (TypeError, ValueError):
                pass
        if meta.get("azimuth_deg") is not None:
            bits.append(f"az {meta['azimuth_deg']}°")
        if meta.get("fusion_decision"):
            bits.append(f"fus {meta['fusion_decision']}")
        if meta.get("civilian_in_radius"):
            bits.append("гражданское в радиусе")
        if not bits and meta.get("episode_id"):
            bits.append("эпизод")
    elif kind == "hb":
        if meta.get("status"):
            bits.append(f"status={meta['status']}")
        if meta.get("rssi_dbm") is not None:
            try:
                bits.append(f"rssi {int(round(float(meta['rssi_dbm'])))}")
            except (TypeError, ValueError):
                bits.append(f"rssi {meta['rssi_dbm']}")
        hs = _fmt_heap_short(meta.get("free_heap"))
        if hs:
            bits.append(f"heap {hs}")
        if meta.get("firmware_version"):
            bits.append(f"fw {meta['firmware_version']}")
        elif meta.get("uptime_s") is not None:
            bits.append(f"up {meta['uptime_s']}s")
    elif kind == "mel":
        nb, nf = meta.get("num_bands"), meta.get("num_frames")
        if nb and nf:
            bits.append(f"{nb}×{nf}")
        pb = meta.get("payload_bytes") or meta.get("wire_bytes")
        if pb is not None:
            bits.append(f"{pb} B")
        if meta.get("class_name"):
            bits.append(str(meta["class_name"]))
        if meta.get("saved_file"):
            bits.append("spectrogram")
    else:
        if meta.get("topic"):
            bits.append(str(meta["topic"]))
        if meta.get("preview"):
            bits.append(str(meta["preview"])[:80])
    if not bits and transport:
        bits.append(key or transport)
    return " · ".join(bits) if bits else (key or "event")


def enrich_event(ev: dict[str, Any]) -> dict[str, Any]:
    """Normalize timeline row: transport/kind/summary/via_label (T1)."""
    out = dict(ev)
    key = out.get("key") if isinstance(out.get("key"), str) else ""
    meta = out.get("meta") if isinstance(out.get("meta"), dict) else {}
    transport, kind = _event_transport_kind(key)
    if key == "episode":
        mv = str(meta.get("via") or out.get("transport") or "")
        if mv == "mqtt":
            transport = "mqtt"
        elif mv == "mixed":
            transport = "pb"
        else:
            transport = "pb" if mv != "mqtt" else "mqtt"
        kind = "det"
    if not transport and out.get("transport"):
        transport = str(out["transport"])
    if not kind and out.get("kind"):
        kind = str(out["kind"])
    out["transport"] = transport
    out["kind"] = kind
    out["via"] = key or out.get("via") or ""
    if key == "episode":
        via_bit = str(meta.get("via") or transport or "").upper()
        out["via_label"] = f"Эпизод · {via_bit}" if via_bit else "Эпизод"
    else:
        out["via_label"] = VIA_LABELS.get(key, out.get("via_label") or "")
    if not out["via_label"] and transport and kind:
        out["via_label"] = f"{transport.upper()} · {kind.upper()}"
    ok = bool(out.get("ok", True))
    err = str(out.get("error") or "")
    out["summary"] = out.get("summary") or build_event_summary(key, meta, ok=ok, error=err)
    ts_ms = meta.get("timestamp_ms")
    if isinstance(ts_ms, (int, float)) and ts_ms > 0:
        out["device_ts_ms"] = int(ts_ms)
    if "parse_ok" not in out:
        out["parse_ok"] = bool(key) and (kind != "" or not transport)
    return out


def _event_meta(meta: dict[str, Any]) -> dict[str, Any]:
    """Урезаем ленту: без band_means/raw/payload / тяжёлых вложений."""
    skip = {"band_means", "raw", "hist16", "_data", "body"}
    out: dict[str, Any] = {}
    for k, v in meta.items():
        if k in skip:
            continue
        # nested dicts — только мелкие (health/fw/pos) без глубокой копипасты
        if isinstance(v, dict) and k in ("health", "fw", "node_position", "bearing", "detection", "target", "extensions", "model", "window"):
            out[k] = v
        elif not isinstance(v, (dict, list)):
            out[k] = v
        elif isinstance(v, list) and len(v) <= 8:
            out[k] = v
    return out
