#!/usr/bin/env python3
"""Protobuf wire codec + PB validators for Nevod Hub (stdlib only)."""
from __future__ import annotations

import math
import struct
from typing import Any

from hub_mel_io import (
    MEL_BANDS_EXPECTED,
    MEL_DATA_MAX,
    MEL_FRAMES_MAX,
    mel_analytics_uint8,
)

NODE_ID_RE_MAX = 16
MIN_UNIX_TS_MS = 1_000_000_000_000

_WT_VARINT = 0
_WT_64 = 1
_WT_LEN = 2
_WT_32 = 5

def _uvarint(n: int) -> bytes:
    out = bytearray()
    while n > 0x7F:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    out.append(n & 0x7F)
    return bytes(out)


def _read_uvarint(buf: bytes, i: int) -> tuple[int, int]:
    shift = 0
    val = 0
    while True:
        if i >= len(buf):
            raise ValueError("truncated_varint")
        b = buf[i]
        i += 1
        val |= (b & 0x7F) << shift
        if not (b & 0x80):
            return val, i
        shift += 7
        if shift > 63:
            raise ValueError("varint_overflow")


def pb_encode(**fields: Any) -> bytes:
    """Encode simple proto3 fields: key = 'N:type' where type in str|bytes|u64|u32|i32|f32|f64|bool."""
    out = bytearray()
    for key, val in fields.items():
        if val is None:
            continue
        num_s, typ = key.split(":")
        num = int(num_s)
        if typ == "str":
            raw = str(val).encode("utf-8")
            out += _uvarint((num << 3) | _WT_LEN)
            out += _uvarint(len(raw)) + raw
        elif typ == "bytes":
            raw = bytes(val)
            out += _uvarint((num << 3) | _WT_LEN)
            out += _uvarint(len(raw)) + raw
        elif typ in ("u64", "u32", "i32"):
            out += _uvarint((num << 3) | _WT_VARINT)
            out += _uvarint(int(val) & ((1 << 64) - 1))
        elif typ == "bool":
            out += _uvarint((num << 3) | _WT_VARINT)
            out += _uvarint(1 if val else 0)
        elif typ == "f32":
            out += _uvarint((num << 3) | _WT_32)
            out += struct.pack("<f", float(val))
        elif typ == "f64":
            out += _uvarint((num << 3) | _WT_64)
            out += struct.pack("<d", float(val))
        else:
            raise ValueError(f"unknown type {typ}")
    return bytes(out)


def pb_decode(buf: bytes) -> dict[int, list[Any]]:
    """Map field_number → list of values (last wins for scalars via helpers)."""
    fields: dict[int, list[Any]] = {}
    i = 0
    n = len(buf)
    while i < n:
        tag, i = _read_uvarint(buf, i)
        fn = tag >> 3
        wt = tag & 7
        if wt == _WT_VARINT:
            v, i = _read_uvarint(buf, i)
            fields.setdefault(fn, []).append(("varint", v))
        elif wt == _WT_64:
            if i + 8 > n:
                raise ValueError("truncated_64")
            fields.setdefault(fn, []).append(("fixed64", buf[i : i + 8]))
            i += 8
        elif wt == _WT_LEN:
            ln, i = _read_uvarint(buf, i)
            if i + ln > n:
                raise ValueError("truncated_len")
            fields.setdefault(fn, []).append(("bytes", buf[i : i + ln]))
            i += ln
        elif wt == _WT_32:
            if i + 4 > n:
                raise ValueError("truncated_32")
            fields.setdefault(fn, []).append(("fixed32", buf[i : i + 4]))
            i += 4
        else:
            raise ValueError(f"bad_wire_type:{wt}")
    return fields


def _last_str(fields: dict[int, list[Any]], fn: int, default: str = "") -> str:
    vals = fields.get(fn) or []
    for kind, v in reversed(vals):
        if kind == "bytes":
            return v.decode("utf-8", errors="replace")
    return default


def _last_bytes(fields: dict[int, list[Any]], fn: int) -> bytes:
    vals = fields.get(fn) or []
    for kind, v in reversed(vals):
        if kind == "bytes":
            return v
    return b""


def _last_varint(fields: dict[int, list[Any]], fn: int, default: int = 0) -> int:
    vals = fields.get(fn) or []
    for kind, v in reversed(vals):
        if kind == "varint":
            return int(v)
    return default


def _last_f32(fields: dict[int, list[Any]], fn: int) -> float | None:
    vals = fields.get(fn) or []
    for kind, v in reversed(vals):
        if kind == "fixed32":
            return struct.unpack("<f", v)[0]
    return None


def _last_f64(fields: dict[int, list[Any]], fn: int) -> float | None:
    vals = fields.get(fn) or []
    for kind, v in reversed(vals):
        if kind == "fixed64":
            return struct.unpack("<d", v)[0]
    return None


def _finite(x: float | None) -> bool:
    return x is not None and math.isfinite(x)


def _finite_prob(x: float | None) -> bool:
    return _finite(x) and 0.0 <= float(x) <= 1.0


def _require_wall_clock_ms(ts: int) -> None:
    if int(ts) < MIN_UNIX_TS_MS:
        raise ValueError("bad_timestamp_ms")


def _canon_node_id(raw: Any) -> str:
    """PB/topic: 746E8C · MQTT body: nevod-746E8C → один ключ 746E8C."""
    if not isinstance(raw, str):
        return ""
    s = raw.strip()
    if not s:
        return ""
    if s.lower().startswith("nevod-"):
        s = s[6:]
    return s[:NODE_ID_RE_MAX]


def _derive_class_ru(class_id: Any, class_name: str) -> str | None:
    """PB не несёт class_ru — как UavLabels.h на плате."""
    name = (class_name or "").strip()
    if name == "UAV":
        return "неизвестный БПЛА"
    by_name = {
        "background": "фон",
        "drone": "коптер",
        "ice_uav": "ДВС-БПЛА",
        "jet_uav": "ТРД-БПЛА",
    }
    if name in by_name:
        return by_name[name]
    try:
        cid = int(class_id) if class_id is not None else None
    except (TypeError, ValueError):
        cid = None
    by_id = {0: "фон", 1: "коптер", 2: "ДВС-БПЛА", 3: "ТРД-БПЛА"}
    if cid in by_id:
        return by_id[cid]
    return None


def validate_detection_pb(raw: bytes) -> dict[str, Any]:
    f = pb_decode(raw)
    node_id = _canon_node_id(_last_str(f, 1))
    if not node_id or len(node_id) > NODE_ID_RE_MAX:
        raise ValueError("invalid_node_id")
    ts = _last_varint(f, 2)
    _require_wall_clock_ms(ts)
    conf = _last_f32(f, 5)
    threat = _last_f32(f, 6)
    if conf is not None and not _finite(conf):
        raise ValueError("non_finite_confidence")
    if threat is not None and not _finite_prob(threat):
        raise ValueError("invalid_threat")
    out: dict[str, Any] = {
        "node_id": node_id,
        "timestamp_ms": ts,
        "class_name": _last_str(f, 3),
        "class_id": _last_varint(f, 4),
        "confidence": conf,
        "threat": threat,
        "threat_rate": _last_f32(f, 7),
        "early_warning": bool(_last_varint(f, 8)),
        "bpf_hz": _last_f32(f, 9),
        "doa_confidence": _last_f32(f, 11),
        "wire_bytes": len(raw),
    }
    # Field 27: explicit presence. proto3 omits azimuth_deg == 0.
    # Old nodes (no flag): non-zero field 10 is a bearing; 0 or absent is not.
    az = _last_f32(f, 10)
    az_valid = bool(_last_varint(f, 27))
    if az_valid:
        out["azimuth_valid"] = True
        out["azimuth_deg"] = 0.0 if az is None else float(az)
    elif az is not None and float(az) != 0.0:
        out["azimuth_deg"] = float(az)
    else:
        out["azimuth_deg"] = None
    ru = _derive_class_ru(out.get("class_id"), out.get("class_name") or "")
    if ru:
        out["class_ru"] = ru
    # Fields 24–26: ICE RPM wire B (23/rotor_hz reserved). Absent → not valid.
    rpm_valid = bool(_last_varint(f, 26))
    out["rpm_valid"] = rpm_valid
    if rpm_valid:
        rp = _last_f32(f, 24)
        bc = _last_varint(f, 25)
        if rp is not None:
            out["rpm"] = rp
        if bc is not None:
            out["blade_count"] = int(bc)
    return out


def validate_heartbeat_pb(raw: bytes) -> dict[str, Any]:
    f = pb_decode(raw)
    node_id = _canon_node_id(_last_str(f, 1))
    if not node_id or len(node_id) > NODE_ID_RE_MAX:
        raise ValueError("invalid_node_id")
    ts = _last_varint(f, 2)
    _require_wall_clock_ms(ts)
    out = {
        "node_id": node_id,
        "timestamp_ms": ts,
        "status": _last_str(f, 3) or "ok",
        "noise_dbfs": _last_f32(f, 5),
        "uptime_s": _last_varint(f, 6),
        "lat": _last_f64(f, 10),
        "lon": _last_f64(f, 11),
        "spl_fast": _last_f32(f, 12),
        "wire_bytes": len(raw),
    }
    # Field 13 = firmware_version (proto C / #130). Absent on older builds → OK.
    fw = _last_str(f, 13)
    if fw:
        out["firmware_version"] = fw[:64]
    xvf = _last_str(f, 14)
    if xvf:
        out["xvf_firmware_version"] = xvf[:16]
    nn = _last_str(f, 15)
    if nn:
        out["nn_model_version"] = nn[:16]
    # Field 16 = install_height_m AGL (0 = unset / absent).
    inst = _last_f32(f, 16)
    if inst is not None and float(inst) > 0.0:
        out["install_height_m"] = float(inst)
        out["alt_ref"] = "agl"
        out["alt_m"] = float(inst)
    return out


def validate_mel_pb(raw: bytes) -> dict[str, Any]:
    f = pb_decode(raw)
    node_id = _canon_node_id(_last_str(f, 1))
    if not node_id or len(node_id) > NODE_ID_RE_MAX:
        raise ValueError("invalid_node_id")
    ts = _last_varint(f, 2)
    bands = _last_varint(f, 3)
    frames = _last_varint(f, 4)
    wire = _last_bytes(f, 5)
    dmin = _last_f32(f, 6)
    dmax = _last_f32(f, 7)
    data_encoding = int(_last_varint(f, 8) or 0)  # 0=raw, 1=gzip, 2=gzip+delta
    frontend_id = _last_str(f, 9) or "logmel_cmn"
    frontend_version = int(_last_varint(f, 10) or 0)
    frontend_params_hash = int(_last_varint(f, 11) or 0)
    gain_epoch = int(_last_varint(f, 12) or 0)
    class_name = _last_str(f, 13) or ""
    class_id = int(_last_varint(f, 14) or 0)
    _require_wall_clock_ms(ts)
    if bands != MEL_BANDS_EXPECTED:
        raise ValueError(f"bad_num_bands:{bands}")
    if frames != MEL_FRAMES_MAX:
        # ML contract: only full 64×401. Partial rings (boot/restart) rejected.
        raise ValueError(f"bad_num_frames:{frames}!=401")
    if not wire:
        raise ValueError("mel_data_empty")
    if len(wire) > MEL_DATA_MAX:
        raise ValueError("mel_data_too_large")
    # enc=3 (delta>>4) destroys real Mel: small temporal deltas → zero nibbles,
    # gunzip+undelta «succeeds» but spectrogram is garbage. Reject — use enc=2.
    if data_encoding == 3:
        raise ValueError("mel_enc3_lossy_disabled")
    need = int(bands) * int(frames)
    if data_encoding in (1, 2):
        import zlib

        # Cap inflate before full gunzip (gzip-bomb DoS).
        max_raw = need
        try:
            dec = zlib.decompressobj(wbits=16 + zlib.MAX_WBITS)
            data = dec.decompress(wire, max_length=max_raw + 1)
            if dec.unconsumed_tail or len(data) > max_raw:
                raise ValueError(f"mel_gunzip_too_big:{len(data)}>{max_raw}")
            leftover = dec.decompress(b"")
            if leftover:
                raise ValueError("mel_gunzip_trailing")
        except zlib.error as e:
            raise ValueError(f"mel_gunzip_fail:{e}") from e
        if data_encoding == 2:
            if len(data) != need:
                raise ValueError(f"mel_size_mismatch:{len(data)}!={need}")
            arr = bytearray(data)
            step = int(bands)
            for i in range(step, need):
                arr[i] = (arr[i] + arr[i - step]) & 0xFF
            data = bytes(arr)
    elif data_encoding == 0:
        data = wire
    else:
        raise ValueError(f"bad_data_encoding:{data_encoding}")
    if len(data) != need:
        raise ValueError(f"mel_size_mismatch:{len(data)}!={need}")
    if not _finite(dmin) or not _finite(dmax):
        raise ValueError("missing_or_non_finite_minmax")
    if float(dmax) < float(dmin):  # type: ignore[arg-type]
        raise ValueError("data_max_lt_min")
    analytics = mel_analytics_uint8(data, bands, frames, float(dmin), float(dmax))
    enc_name = {
        0: "uint8_minmax",
        1: "uint8_minmax_gzip",
        2: "uint8_minmax_gzip_delta",
        3: "uint8_minmax_gzip_delta_u4",
    }.get(data_encoding, "uint8_minmax")
    return {
        "node_id": node_id,
        "timestamp_ms": ts,
        "num_bands": bands,
        "num_frames": frames,
        "data_min": dmin,
        "data_max": dmax,
        "payload_bytes": len(data),
        "wire_payload_bytes": len(wire),
        "wire_bytes": len(raw),
        "data_encoding": data_encoding,
        "encoding": enc_name,
        "frontend_id": frontend_id,
        "frontend_version": frontend_version,
        "frontend_params_hash": frontend_params_hash,
        "gain_epoch": gain_epoch,
        "class_name": class_name,
        "class_id": class_id,
        "_data": data,
        **analytics,
    }
