#!/usr/bin/env python3
"""MQTT JSON parse + DET↔HB join for Nevod Hub."""
from __future__ import annotations

from datetime import datetime, timezone
from typing import TYPE_CHECKING, Any

from hub_pb import NODE_ID_RE_MAX, _canon_node_id, pb_encode

if TYPE_CHECKING:
    from hub_runtime import NodeRecord

JOIN_HB_MAX_AGE_MS = 120_000  # DET←HB join window


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _extract_fw_version(j: dict[str, Any]) -> str | None:
    fw = j.get("fw")
    if isinstance(fw, dict):
        v = fw.get("version")
        if isinstance(v, str) and v.strip():
            return v.strip()[:64]
    v = j.get("firmware_version")
    if isinstance(v, str) and v.strip():
        return v.strip()[:64]
    return None


def _apply_firmware_version(
    node: "NodeRecord", version: Any, at: str | None = None
) -> None:
    if not isinstance(version, str):
        return
    ver = version.strip()[:64]
    if not ver:
        return
    node.firmware_version = ver
    node.firmware_version_at = at or _now_iso()


def _extract_xvf_version(j: dict[str, Any]) -> str | None:
    fw = j.get("fw")
    if isinstance(fw, dict):
        v = fw.get("xvf")
        if isinstance(v, str) and v.strip():
            return v.strip()[:16]
    v = j.get("xvf_firmware_version")
    if isinstance(v, str) and v.strip():
        return v.strip()[:16]
    return None


def _apply_xvf_firmware_version(
    node: "NodeRecord", version: Any, at: str | None = None
) -> None:
    if not isinstance(version, str):
        return
    ver = version.strip()[:16]
    if not ver:
        return
    node.xvf_firmware_version = ver
    node.xvf_firmware_version_at = at or _now_iso()


def _extract_nn_version(j: dict[str, Any]) -> str | None:
    fw = j.get("fw")
    if isinstance(fw, dict):
        v = fw.get("nn")
        if isinstance(v, str) and v.strip():
            return v.strip()[:16]
    v = j.get("nn_model_version")
    if isinstance(v, str) and v.strip():
        return v.strip()[:16]
    return None


def _apply_nn_model_version(
    node: "NodeRecord", version: Any, at: str | None = None
) -> None:
    if not isinstance(version, str):
        return
    ver = version.strip()[:16]
    if not ver:
        return
    node.nn_model_version = ver
    node.nn_model_version_at = at or _now_iso()


def _mqtt_ts_ms(j: dict[str, Any]) -> int:
    raw = j.get("timestamp_ms")
    if isinstance(raw, (int, float)) and raw > 0:
        v = float(raw)
        return int(v if v > 1e12 else v * 1000.0)
    ts = j.get("ts")
    if isinstance(ts, str) and ts.strip():
        try:
            d = datetime.fromisoformat(ts.strip().replace("Z", "+00:00"))
            return int(d.timestamp() * 1000.0)
        except ValueError:
            return 0
    return 0


def mqtt_is_lora(j: dict[str, Any] | None) -> bool:
    """Шлюз помечает эфир: extensions.via=lora_gw (FLR). path=lora_mqtt — запасное имя."""
    if not isinstance(j, dict):
        return False
    ext = j.get("extensions")
    if isinstance(ext, dict) and str(ext.get("via") or "").strip() == "lora_gw":
        return True
    return str(j.get("path") or "").strip() == "lora_mqtt"


def _finite_num(val: Any) -> float | None:
    if isinstance(val, bool) or not isinstance(val, (int, float)):
        return None
    x = float(val)
    if x != x or x in (float("inf"), float("-inf")):
        return None
    return x


def lora_mqtt_pb(kind: str, j: dict[str, Any]) -> tuple[str, bytes] | None:
    """LoRa MQTT → protobuf облака. У такого узла другого пакета нет."""
    if kind not in ("heartbeat", "detection") or not mqtt_is_lora(j):
        return None
    node_id = _mqtt_node_id(j, "")
    ts = _mqtt_ts_ms(j)
    if ts < 1_000_000_000_000:
        ts = int(datetime.now(timezone.utc).timestamp() * 1000.0)
    if not node_id:
        return None
    if kind == "heartbeat":
        rec = _parse_mqtt_heartbeat(j, node_id, "")
        fields: dict[str, Any] = {
            "1:str": node_id[:16],
            "2:u64": ts,
            "3:str": str(rec.get("status") or "ok")[:16],
        }
        noise = _finite_num(rec.get("noise_dbfs"))
        if noise is not None:
            fields["5:f32"] = noise
        up = rec.get("uptime_s")
        if isinstance(up, (int, float)) and not isinstance(up, bool):
            fields["6:i32"] = int(up)
        lat, lon = _finite_num(rec.get("lat")), _finite_num(rec.get("lon"))
        if lat is not None and lon is not None:
            fields["10:f64"] = lat
            fields["11:f64"] = lon
        spl = _finite_num(rec.get("spl_fast"))
        if spl is not None:
            fields["12:f32"] = spl
        fw = rec.get("firmware_version")
        if isinstance(fw, str) and fw.strip():
            fields["13:str"] = fw.strip()[:32]
        xvf = rec.get("xvf_firmware_version")
        if isinstance(xvf, str) and xvf.strip():
            fields["14:str"] = xvf.strip()[:16]
        nn = rec.get("nn_model_version")
        if isinstance(nn, str) and nn.strip():
            fields["15:str"] = nn.strip()[:16]
        inst = _finite_num(rec.get("install_height_m"))
        if inst is not None and inst > 0.0:
            fields["16:f32"] = inst
        return "/api/v1/pb/ReportHeartbeat", pb_encode(**fields)

    rec = _parse_mqtt_detection(j, node_id, "")
    fields = {
        "1:str": node_id[:16],
        "2:u64": ts,
        "3:str": str(rec.get("class_name") or "")[:32],
    }
    cid = rec.get("class_id")
    if isinstance(cid, (int, float)) and not isinstance(cid, bool):
        fields["4:i32"] = int(cid)
    for key, src in (
        ("5:f32", "confidence"),
        ("6:f32", "threat"),
        ("7:f32", "threat_rate"),
        ("9:f32", "bpf_hz"),
        ("11:f32", "doa_confidence"),
    ):
        num = _finite_num(rec.get(src))
        if num is not None:
            fields[key] = num
    # proto3 опускает float 0. Поле 27 — тот же контракт, что у прошивки:
    # север 0° остаётся пеленгом, отсутствие ключа — нет пеленга.
    az = _finite_num(rec.get("azimuth_deg"))
    if az is not None:
        fields["10:f32"] = az
        fields["27:bool"] = True
    if rec.get("early_warning"):
        fields["8:bool"] = True
    if rec.get("rpm_valid"):
        fields["26:bool"] = True
        rpm = _finite_num(rec.get("rpm"))
        if rpm is not None:
            fields["24:f32"] = rpm
        bc = rec.get("blade_count")
        if isinstance(bc, (int, float)) and not isinstance(bc, bool):
            fields["25:i32"] = int(bc)
    return "/api/v1/pb/ReportDetection", pb_encode(**fields)


def _mqtt_node_id(j: dict[str, Any], topic_node: str) -> str:
    nid = j.get("node_id")
    if isinstance(nid, str) and nid.strip():
        return _canon_node_id(nid)
    return _canon_node_id(topic_node)


def _json_obj(v: Any) -> dict[str, Any]:
    return dict(v) if isinstance(v, dict) else {}


def _parse_mqtt_heartbeat(j: dict[str, Any], node_id: str, now: str) -> dict[str, Any]:
    """Полный nevod.heartbeat.v1 → last_heartbeat (+ flat aliases для UI/join)."""
    h = _json_obj(j.get("health"))
    pos = _json_obj(j.get("node_position"))
    fw = _json_obj(j.get("fw"))
    rssi = h.get("rssi_dbm")
    heap = h.get("free_heap_bytes")
    if heap is None:
        heap = h.get("free_heap")
    heap_min = h.get("free_heap_min")
    if heap_min is None:
        heap_min = h.get("free_heap_min_bytes")
    uptime = h.get("uptime_s")
    if uptime is None:
        uptime = fw.get("uptime_s")
    ver = _extract_fw_version(j)
    rec: dict[str, Any] = {
        "node_id": node_id,
        "via": "lora_gw" if mqtt_is_lora(j) else "mqtt_heartbeat",
        "recv_at": now,
        "schema": j.get("schema"),
        "extensions": j.get("extensions") if isinstance(j.get("extensions"), dict) else None,
        "ts": j.get("ts"),
        "timestamp_ms": _mqtt_ts_ms(j),
        "status": j.get("status"),
        "noise_dbfs": j.get("noise_dbfs"),
        "spl_fast": j.get("spl_fast"),
        "spl_dbfs": j.get("spl_dbfs"),
        "device_type": j.get("device_type"),
        "node_position": pos or None,
        "health": h or None,
        "fw": fw or None,
        "rssi_dbm": rssi,
        "free_heap": heap,
        "free_heap_min": heap_min,
        "uptime_s": uptime,
        "temp_c": h.get("temp_c"),
        "cpu_mhz": h.get("cpu_mhz"),
        "cpu_load": h.get("cpu_load"),
        "infer_ms": h.get("infer_ms"),
        "arena_used_bytes": h.get("arena_used_bytes"),
        "lat": pos.get("lat"),
        "lon": pos.get("lon"),
        "install_height_m": pos.get("install_height_m"),
        "alt_ref": pos.get("alt_ref"),
        # tip MSL only if legacy; prefer install AGL for acoustics / 3D (Hub + DEM)
        "alt_m": (
            pos.get("install_height_m")
            if pos.get("install_height_m") is not None
            else pos.get("alt_m")
        ),
        "gps_fix": pos.get("fix"),
        "gps_valid": pos.get("valid"),
        "gps_source": pos.get("source"),
    }
    rec.update(parse_loitering(j))
    if ver:
        rec["firmware_version"] = ver
    xvf = _extract_xvf_version(j)
    if xvf:
        rec["xvf_firmware_version"] = xvf
    nn = _extract_nn_version(j)
    if nn:
        rec["nn_model_version"] = nn
    return rec


def _parse_mqtt_detection(j: dict[str, Any], node_id: str, now: str) -> dict[str, Any]:
    """Полный nevod.detection.v1 → last_detection (+ flat для join/UI)."""
    det_blk = _json_obj(j.get("detection"))
    target = _json_obj(j.get("target"))
    bearing = _json_obj(j.get("bearing"))
    ext = _json_obj(j.get("extensions"))
    hps = _json_obj(ext.get("hps"))
    rpm = _json_obj(ext.get("rpm"))
    fus = _json_obj(ext.get("fusion"))
    pos = _json_obj(j.get("node_position"))
    fw = _json_obj(j.get("fw"))
    model = _json_obj(j.get("model"))
    window = _json_obj(j.get("window"))
    class_name = (
        det_blk.get("class")
        or target.get("class")
        or det_blk.get("class_ru")
        or ""
    )
    class_id = det_blk.get("class_id")
    if class_id is None:
        class_id = target.get("class_id")
    confidence = det_blk.get("p")
    if confidence is None:
        confidence = target.get("p")
    threat = det_blk.get("threat")
    if threat is None and fus:
        threat = fus.get("threat")
    ver = _extract_fw_version(j)
    rec: dict[str, Any] = {
        "node_id": node_id,
        "via": "lora_gw" if mqtt_is_lora(j) else "mqtt_detection",
        "recv_at": now,
        "schema": j.get("schema"),
        "msg_id": j.get("msg_id"),
        "ts": j.get("ts"),
        "timestamp_ms": _mqtt_ts_ms(j),
        "window": window or None,
        "target": target or None,
        "detection": det_blk or None,
        "bearing": bearing or None,
        "node_position": pos or None,
        "model": model or None,
        "fw": fw or None,
        "extensions": ext or None,
        "class_name": class_name,
        "class_ru": det_blk.get("class_ru")
        or target.get("class_ru")
        or target.get("label_ru"),
        "class_id": class_id,
        "confidence": confidence,
        "p": confidence,
        "threat": threat,
        "threat_rate": det_blk.get("threat_rate")
        if det_blk.get("threat_rate") is not None
        else fus.get("threat_rate"),
        "early_warning": bool(
            det_blk.get("early_warning") or fus.get("early_warning")
        ),
        "confirmed": det_blk.get("confirmed"),
        "alarm_tier": det_blk.get("alarm_tier") or "none",
        # Нет поля (LoRa-эфир, старые узлы) → None, а не «тон не совпал».
        "tone_agreed": (
            bool(det_blk["tone_agreed"])
            if det_blk.get("tone_agreed") is not None
            else None
        ),
        "probs": det_blk.get("probs") or target.get("probs"),
        "n_of_m": det_blk.get("n_of_m"),
        "n_of_m_hits": det_blk.get("n_of_m_hits"),
        "bpf_hz": det_blk.get("bpf_hz") or hps.get("bpf_hz"),
        "hps_score": hps.get("score"),
        "rpm_valid": bool(rpm.get("valid")),
        "rpm": rpm.get("rpm") if rpm.get("valid") else None,
        "blade_count": rpm.get("blade_count") if rpm.get("valid") else None,
        "azimuth_deg": bearing.get("azimuth_deg"),
        "azimuth_deg_raw": bearing.get("azimuth_deg_raw"),
        # Прошивка с bearing.n: confidence = R̄ окна сканера; раньше — энергия речи.
        "doa_confidence": bearing.get("confidence"),
        "doa_sigma_deg": bearing.get("sigma_deg"),
        "doa_n": bearing.get("n"),
        "doa_age_ms": bearing.get("az_age_ms"),
        "heading_ref": bearing.get("heading_ref"),
        # Только старые узлы: 0° и 24° там константы, не измерения.
        "elevation_deg": bearing.get("elevation_deg"),
        "beam_width_deg": bearing.get("beam_width_deg"),
        "fusion_decision": fus.get("nn_raw_top")
        or fus.get("decision")
        or class_name
        or None,
        "nn_raw_top": fus.get("nn_raw_top") or fus.get("decision") or class_name or None,
        "fusion_threat": fus.get("threat"),
        "detection_layers": ext.get("detection_layers"),
        "device_type": ext.get("device_type") or j.get("device_type"),
        "trust_weight": ext.get("trust_weight"),
        "lat": pos.get("lat"),
        "lon": pos.get("lon"),
        "install_height_m": pos.get("install_height_m"),
        "alt_ref": pos.get("alt_ref"),
        "alt_m": (
            pos.get("install_height_m")
            if pos.get("install_height_m") is not None
            else pos.get("alt_m")
        ),
    }
    rec.update(parse_loitering(det_blk))
    if ver:
        rec["firmware_version"] = ver
    return rec


def parse_loitering(blk: dict[str, Any]) -> dict[str, Any]:
    """Firmware sends loitering (+ presence_s) only while true: absence = false."""
    on = blk.get("loitering") is True
    pres = blk.get("presence_s") if on else None
    if isinstance(pres, bool) or not isinstance(pres, (int, float)):
        pres = None
    return {"loitering": on, "presence_s": int(pres) if pres is not None else None}


def join_detection_with_hb(det: dict[str, Any], hb: dict[str, Any] | None) -> dict[str, Any]:
    """Enrich DET view with node state from last HB (same node_id, |Δt| ≤ window)."""
    out = dict(det)
    out["joined"] = False
    if not hb:
        out["join_reason"] = "no_heartbeat"
        return out
    ts_d = int(det.get("timestamp_ms") or 0)
    ts_h = int(hb.get("timestamp_ms") or 0)
    if ts_d <= 0 or ts_h <= 0:
        out["join_reason"] = "missing_ts"
        return out
    age_ms = abs(ts_d - ts_h)
    out["hb_age_ms"] = age_ms  # |t_DET − t_HB|, мс
    if age_ms > JOIN_HB_MAX_AGE_MS:
        out["join_reason"] = "hb_stale"
        return out
    out["joined"] = True
    out["join_reason"] = "ok"
    out["hb_timestamp_ms"] = ts_h
    for k in ("spl_fast", "noise_dbfs", "lat", "lon", "uptime_s", "status"):
        if hb.get(k) is not None and out.get(k) is None:
            out[k] = hb.get(k)
    return out
