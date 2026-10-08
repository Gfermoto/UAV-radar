#!/usr/bin/env python3
"""msh ServiceEnvelope / compact PHR → nevod.*.v1 + Mel 64×401 store.

Слой B (эфир PRIVATE_APP на msh/+/2/e/#). Не lab inject §9.
"""
from __future__ import annotations

import json
import time
from datetime import datetime, timezone
from typing import Any

from hub_mel_io import mel_analytics_uint8, save_mel_payload
from hub_pb import _canon_node_id, pb_encode

PHR_HB = 0x12
PHR_DET = 0x11
PHR_MEL = 0x14
PORT_PRIVATE_APP = 256
MEL_BANDS = 64
MEL_FRAMES = 401
MEL_RAW = 25664
MEL_CHUNK_DATA = 180
MEL_QUANT_SPAN_NAT = 8.0  # firmware NetConfig.h — нет floor на эфире
CLASS = {1: "drone", 2: "ice_uav", 3: "jet_uav"}
CLASS_RU = {1: "коптер", 2: "ДВС-БПЛА", 3: "ТРД-БПЛА"}
SKU = {1: "diy", 2: "light", 3: "pro", 4: "field"}
FLAG_HAS_GPS = 0x01
FLAG_HAS_TIME = 0x02
FLAG_HAS_TELEM = 0x04
FLAG_HAS_ALT = 0x08
FLAG_LOITERING = 0x10  # HB state bit (no payload)
DET_LOITERING = 0x04


def _u16be(b: bytes, i: int) -> int:
    return (b[i] << 8) | b[i + 1]


def _u32be(b: bytes, i: int) -> int:
    return (b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3]


def _i32be(b: bytes, i: int) -> int:
    v = _u32be(b, i)
    return v - 0x100000000 if v >= 0x80000000 else v


def _i8(u: int) -> int:
    return int(u) - 256 if int(u) >= 128 else int(u)


def _varint(buf: bytes, i: int) -> tuple[int, int]:
    v = 0
    shift = 0
    while i < len(buf):
        x = buf[i]
        i += 1
        v |= (x & 0x7F) << shift
        if (x & 0x80) == 0:
            return v, i
        shift += 7
        if shift > 63:
            break
    raise ValueError("bad_varint")


def iter_fields(buf: bytes) -> list[tuple[int, int, bytes | int]]:
    out: list[tuple[int, int, bytes | int]] = []
    i = 0
    n = len(buf)
    while i < n:
        key, i = _varint(buf, i)
        field, wt = key >> 3, key & 7
        if wt == 0:
            v, i = _varint(buf, i)
            out.append((field, wt, v))
        elif wt == 1:
            if i + 8 > n:
                break
            out.append((field, wt, buf[i : i + 8]))
            i += 8
        elif wt == 2:
            ln, i = _varint(buf, i)
            if i + ln > n:
                break
            out.append((field, wt, buf[i : i + ln]))
            i += ln
        elif wt == 5:
            if i + 4 > n:
                break
            out.append((field, wt, buf[i : i + 4]))
            i += 4
        else:
            break
    return out


def extract_private_app(raw: bytes) -> bytes | None:
    """ServiceEnvelope.packet / FromRadio.packet → Data.payload if PRIVATE_APP."""
    if not raw:
        return None
    if raw[0] in (PHR_HB, PHR_DET, PHR_MEL):
        return raw
    try:
        top = iter_fields(raw)
    except ValueError:
        return None
    packet = None
    for field, wt, val in top:
        if wt == 2 and field in (1, 2) and isinstance(val, (bytes, bytearray)):
            packet = bytes(val)
            break
    if packet is None:
        return None
    decoded = None
    for field, wt, val in iter_fields(packet):
        if wt == 2 and field == 4 and isinstance(val, (bytes, bytearray)):
            decoded = bytes(val)
            break
    if decoded is None:
        return None
    port = 0
    payload = None
    for field, wt, val in iter_fields(decoded):
        if wt == 0 and field == 1 and isinstance(val, int):
            port = val
        if wt == 2 and field == 2 and isinstance(val, (bytes, bytearray)):
            payload = bytes(val)
    if port != PORT_PRIVATE_APP or not payload:
        return None
    return payload


def node_hex(node_id: int) -> str:
    """Compact u32 = MAC[2..5] (spec). Hub/Eth/PB = MAC[3..5] = low 24 bits.

    0x042C46E4 (Light air) → 2C46E4 (тот же узел, что Eth). 0x00746E8C → 746E8C.
    §9 inject DD3DE12A сюда не ходит (JSON note_mqtt, не compact).
    """
    return f"{int(node_id) & 0xFFFFFF:06X}"


def _iso(unix_s: int | None, fallback_ms: int | None) -> str:
    if unix_s and unix_s >= 1_000_000_000:
        dt = datetime.fromtimestamp(unix_s, tz=timezone.utc)
        return dt.strftime("%Y-%m-%dT%H:%M:%S.000Z")
    if fallback_ms and fallback_ms > 1_000_000_000_000:
        dt = datetime.fromtimestamp(fallback_ms / 1000.0, tz=timezone.utc)
        return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{int(fallback_ms % 1000):03d}Z"
    dt = datetime.now(timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.000Z")


def decode_hb(wire: bytes) -> dict[str, Any] | None:
    if len(wire) < 12 or wire[0] != PHR_HB:
        return None
    node = _u32be(wire, 1)
    flags = wire[11]
    unix_s = None
    lat = lon = None
    o = 12
    if flags & FLAG_HAS_TIME:
        if len(wire) < o + 4:
            return None
        unix_s = _u32be(wire, o)
        o += 4
    if flags & FLAG_HAS_GPS:
        if len(wire) < o + 8:
            return None
        lat = _i32be(wire, o) / 1e7
        lon = _i32be(wire, o + 4) / 1e7
        o += 8
    uptime_s = int(_u16be(wire, 9)) * 60
    health: dict[str, Any] = {"uptime_s": uptime_s}
    device_type = None
    fw: dict[str, Any] | None = None
    if flags & FLAG_HAS_TELEM:
        if len(wire) < o + 8:
            return None
        heap_kb = wire[o]
        heap_min_kb = wire[o + 1]
        health["free_heap"] = int(heap_kb) * 1024
        health["free_heap_min"] = int(heap_min_kb) * 1024
        health["temp_c"] = float(_i8(wire[o + 2]))
        infer_cs = wire[o + 7]
        if infer_cs:
            health["infer_ms"] = int(infer_cs) * 10
        device_type = SKU.get(int(wire[o + 3]))
        ver = f"{int(wire[o + 4])}.{int(wire[o + 5])}.{int(wire[o + 6])}"
        fw = {"version": ver, "uptime_s": uptime_s}
        o += 8
    alt_m = None
    if flags & FLAG_HAS_ALT:
        if len(wire) < o + 2:
            return None
        raw_dm = _u16be(wire, o)
        if raw_dm >= 0x8000:
            raw_dm -= 0x10000
        alt_m = raw_dm / 10.0
    status = {0: "ok", 1: "degraded", 2: "fault"}.get(wire[6], "ok")
    nid = node_hex(node)
    body: dict[str, Any] = {
        "schema": "nevod.heartbeat.v1",
        "node_id": nid,
        "ts": _iso(unix_s, None),
        "status": status,
        # Байт эфира — dBFS (−96…0), не калиброванный SPL. spl_fast только у MQTT/TCP.
        "spl_dbfs": float(wire[7]) - 96.0,
        "noise_dbfs": float(wire[8]) - 96.0,
        "device_type": device_type,
        "health": health,
        "fw": fw,
        "extensions": {"via": "lora_gw", "clock": "device" if unix_s else "gw_rx"},
    }
    if fw:
        body["firmware_version"] = fw["version"]
    if lat is not None and lon is not None:
        pos = {"lat": lat, "lon": lon, "source": "lora_gnss", "valid": True}
        if alt_m is not None:
            # На эфире нет флага AGL/MSL: число не выдаём за мачту или уровень моря.
            pos["alt_m"] = alt_m
            pos["alt_ref"] = "node"
        body["node_position"] = pos
    if unix_s:
        body["timestamp_ms"] = int(unix_s) * 1000
    if flags & FLAG_LOITERING:
        body["loitering"] = True
    return body


def decode_det(wire: bytes, hb_unix: int | None = None) -> dict[str, Any] | None:
    if len(wire) < 18 or wire[0] != PHR_DET:
        return None
    cid = wire[8]
    if cid == 0 or cid not in CLASS:
        return None
    node = _u32be(wire, 1)
    dt_s = _u16be(wire, 6)
    p = wire[9] / 255.0
    threat = wire[10] / 255.0
    az = _u16be(wire, 11)
    flags = wire[16]
    n = (wire[14] >> 4) & 0x0F
    m = wire[14] & 0x0F
    nid = node_hex(node)
    unix = (hb_unix + dt_s) if hb_unix else None
    cls = CLASS[cid]
    # MeshAirCodec: kDetConfirmed=0x01 (tone-agreed publish), kDetEarly=0x02
    # (alarm_tier=early). Нет отдельного tone_agreed на эфире — confirmed-бит
    # = toneOut.confirmed из sidecar, не «голый» NN publish. Без early и без
    # confirmed → alarm_tier=none (не выдумываем confirmed).
    det_confirmed = bool(flags & 0x01)
    det_early = bool(flags & 0x02)
    if det_confirmed:
        alarm_tier = "confirmed"
    elif det_early:
        alarm_tier = "early"
    else:
        alarm_tier = "none"
    body: dict[str, Any] = {
        "schema": "nevod.detection.v1",
        "node_id": nid,
        "ts": _iso(unix, None),
        "detection": {
            "class_id": cid,
            "class": cls,
            "class_ru": CLASS_RU[cid],
            "p": p,
            "threat": threat,
            "confirmed": det_confirmed,
            "early_warning": det_early,
            "alarm_tier": alarm_tier,
            **({"loitering": True} if flags & DET_LOITERING else {}),
            "n_of_m": [n, m],
            "n_of_m_hits": int(wire[15]),
        },
        "target": {
            "class": cls,
            "class_id": cid,
            "class_ru": CLASS_RU[cid],
            "label_ru": CLASS_RU[cid],
        },
        "bearing": {
            "azimuth_deg": None if az == 0xFFFF else int(az),
            "confidence": wire[13] / 255.0,
            "source": "nevod_doa",
        },
        "window": {"sec": 4.0, "hop_sec": 2.0},
        "extensions": {
            "via": "lora_gw",
            "fusion": {"nn_raw_top": cls, "decision": cls, "threat": threat},
        },
    }
    if unix:
        body["timestamp_ms"] = int(unix) * 1000
    return body


def decode_mel_header(wire: bytes) -> dict[str, Any] | None:
    if len(wire) < 16 or wire[0] != PHR_MEL:
        return None
    return {
        "node_id": _u32be(wire, 1),
        "hb_seq": wire[5],
        "dt_s": _u16be(wire, 6),
        "bands": wire[8],
        "frames": _u16be(wire, 9),
        "chunk_i": wire[11],
        "chunk_n": wire[12],
        "raw_len": _u16be(wire, 13),
        "flags": wire[15],
        "data": wire[16:],
    }


class MelAssembler:
    """Один кадр на узел. Replay с тем же hb_seq+dt_s доклеивает дыры 120/143."""

    TIMEOUT_S = 1200.0

    def __init__(self) -> None:
        self._sessions: dict[int, dict[str, Any]] = {}

    def add(self, hdr: dict[str, Any]) -> bytes | None:
        if hdr["bands"] != MEL_BANDS or hdr["frames"] != MEL_FRAMES:
            return None
        if hdr["raw_len"] != MEL_RAW:
            return None
        if hdr["flags"] & 0x01:
            return None
        if hdr["chunk_n"] < 1 or hdr["chunk_i"] >= hdr["chunk_n"]:
            return None
        nid = int(hdr["node_id"])
        now = time.monotonic()
        ses = self._sessions.get(nid)
        # Узел держит hb_seq+dt_s на всю сессию (и на replay); любая смена —
        # новая Mel, иначе две сессии склеиваются в один кадр.
        if ses is not None and (
            ses.get("hb") != hdr["hb_seq"] or ses.get("dt") != hdr["dt_s"]
        ):
            print(
                f"[hub] mel reset node={node_hex(nid)} "
                f"hb={ses.get('hb')}→{hdr['hb_seq']} dt={ses.get('dt')}→{hdr['dt_s']}",
                flush=True,
            )
            del self._sessions[nid]
            ses = None
        if ses is not None and (now - float(ses.get("t0", now))) > self.TIMEOUT_S:
            n = int(ses.get("n") or 0)
            missing = sorted(set(range(n)) - set(ses.get("got") or []))
            miss_s = ",".join(str(x) for x in missing[:16])
            if len(missing) > 16:
                miss_s += f"…+{len(missing) - 16}"
            print(
                f"[hub] mel timeout keep node={node_hex(nid)} "
                f"hb={ses.get('hb')} {len(ses.get('got') or [])}/{n} "
                f"miss={miss_s}",
                flush=True,
            )
            ses["t0"] = now
        if ses is None:
            ses = {
                "got": set(),
                "buf": bytearray(MEL_RAW),
                "n": hdr["chunk_n"],
                "t0": now,
                "hb": hdr["hb_seq"],
                "dt": hdr["dt_s"],
            }
            self._sessions[nid] = ses
            print(
                f"[hub] mel start node={node_hex(nid)} "
                f"hb={hdr['hb_seq']} n={hdr['chunk_n']}",
                flush=True,
            )
        i = int(hdr["chunk_i"])
        if i in ses["got"]:
            return None
        off = i * MEL_CHUNK_DATA
        data = hdr["data"]
        need = min(MEL_CHUNK_DATA, MEL_RAW - off)
        if need <= 0 or len(data) < need:
            print(
                f"[hub] mel skip short node={node_hex(nid)} "
                f"i={i} need={need} got={len(data)}",
                flush=True,
            )
            return None
        ses["buf"][off : off + need] = data[:need]
        ses["got"].add(i)
        got = len(ses["got"])
        if got == 1 or got % 20 == 0 or got >= ses["n"]:
            print(
                f"[hub] mel {node_hex(nid)} "
                f"hb={hdr['hb_seq']} {got}/{ses['n']}",
                flush=True,
            )
        if got >= ses["n"]:
            raw = bytes(ses["buf"])
            del self._sessions[nid]
            return raw
        return None

    def progress(self) -> dict[str, dict[str, int]]:
        """Незавершённые сессии: узел → got/n. Частичный кадр наружу не отдаём."""
        out: dict[str, dict[str, int]] = {}
        for nid, ses in list(self._sessions.items()):
            out[node_hex(nid)] = {
                "got": len(ses.get("got") or ()),
                "n": int(ses.get("n") or 0),
            }
        return out


_ASSEMBLER = MelAssembler()
_HB_UNIX: dict[int, int] = {}
_MEL_RX = 0
_MEL_OK = 0


def mel_progress() -> dict[str, dict[str, int]]:
    return _ASSEMBLER.progress()


def msh_topics(root: str | None = None) -> tuple[str, ...]:
    """Один фильтр. Именованный корень и msh/+/2/e/# совпадают и удваивают on_message."""
    del root
    return ("msh/+/2/e/#",)


def handle_compact(
    payload: bytes, *, now_ms: int | None = None
) -> list[tuple[str, dict[str, Any] | bytes]]:
    """→ list of ('heartbeat'|'detection'|'mel', json-or-raw)."""
    out: list[tuple[str, dict[str, Any] | bytes]] = []
    if not payload:
        return out
    phr = payload[0]
    if phr == PHR_HB:
        hb = decode_hb(payload)
        if hb:
            nid = int(_u32be(payload, 1))
            if payload[11] & 0x02 and len(payload) >= 16:
                _HB_UNIX[nid] = _u32be(payload, 12)
            out.append(("heartbeat", hb))
        return out
    if phr == PHR_DET:
        nid = int(_u32be(payload, 1))
        det = decode_det(payload, _HB_UNIX.get(nid))
        if det:
            out.append(("detection", det))
        return out
    if phr == PHR_MEL:
        global _MEL_RX, _MEL_OK
        _MEL_RX += 1
        hdr = decode_mel_header(payload)
        if not hdr:
            if _MEL_RX == 1 or _MEL_RX % 20 == 0:
                print(
                    f"[hub] 0x14 rx={_MEL_RX} ok={_MEL_OK} decode=fail n={len(payload)}",
                    flush=True,
                )
            return out
        raw = _ASSEMBLER.add(hdr)
        if raw is None:
            if _MEL_RX == 1 or _MEL_RX % 20 == 0:
                print(f"[hub] 0x14 rx={_MEL_RX} ok={_MEL_OK}", flush=True)
        if raw is not None and len(raw) == MEL_RAW:
            _MEL_OK += 1
            out.append(("mel", raw))
            out.append(
                (
                    "mel_meta",
                    {
                        "node_id": node_hex(int(hdr["node_id"])),
                        "hb_seq": hdr["hb_seq"],
                        "dt_s": hdr["dt_s"],
                        "chunk_n": int(hdr["chunk_n"]),
                    },
                )
            )
        return out
    return out


def handle_msh_payload(topic: str, raw: bytes) -> list[tuple[str, dict[str, Any] | bytes]]:
    (void_topic := topic)
    del void_topic
    compact = extract_private_app(raw)
    if not compact:
        return []
    return handle_compact(compact)


def apply_to_state(state: Any, topic: str, raw: bytes) -> bool:
    items = handle_msh_payload(topic, raw)
    if not items:
        return False
    mel_raw: bytes | None = None
    mel_meta: dict[str, Any] | None = None
    for kind, body in items:
        if kind == "mel" and isinstance(body, (bytes, bytearray)):
            mel_raw = bytes(body)
        elif kind == "mel_meta" and isinstance(body, dict):
            mel_meta = body
        elif kind in ("heartbeat", "detection") and isinstance(body, dict):
            nid = _canon_node_id(str(body.get("node_id") or "")) or str(
                body.get("node_id") or ""
            )
            sub = "heartbeat" if kind == "heartbeat" else "detection"
            state.note_mqtt(f"nevod/{nid}/{sub}", json.dumps(body).encode())
    if mel_raw is not None and len(mel_raw) == MEL_RAW:
        nid = _canon_node_id(str((mel_meta or {}).get("node_id") or "")) or "unknown"
        ts = int(time.time() * 1000)
        dmin, dmax = 0.0, MEL_QUANT_SPAN_NAT
        cls = None
        cid = None
        nodes = getattr(state, "nodes", None)
        if isinstance(nodes, dict):
            node = nodes.get(nid)
            det = getattr(node, "last_detection", None) if node is not None else None
            if isinstance(det, dict):
                cls = det.get("class_name") or None
                cid = det.get("class_id")
        analytics = mel_analytics_uint8(mel_raw, MEL_BANDS, MEL_FRAMES, dmin, dmax)
        saved = save_mel_payload(
            data=mel_raw,
            node_id=nid,
            timestamp_ms=ts,
            num_bands=MEL_BANDS,
            num_frames=MEL_FRAMES,
            encoding="uint8_minmax",
            data_min=dmin,
            data_max=dmax,
            data_encoding=0,
            wire_payload_bytes=len(mel_raw),
            class_name=str(cls) if cls else None,
            class_id=int(cid) if isinstance(cid, (int, float)) and not isinstance(cid, bool) else None,
        )
        if hasattr(state, "mark"):
            meta = {
                **analytics,
                **saved,
                "node_id": nid,
                "timestamp_ms": ts,
                "num_bands": MEL_BANDS,
                "num_frames": MEL_FRAMES,
                "encoding": "uint8_minmax",
                "data_min": dmin,
                "data_max": dmax,
                "data_encoding": 0,
                "payload_bytes": len(mel_raw),
                "via": "lora_gw",
                "lora_chunks": int((mel_meta or {}).get("chunk_n") or 0) or None,
            }
            state.mark(
                "pb_mel",
                ok=True,
                meta=meta,
                transport="lora",
                body_len=len(mel_raw),
            )
        from hub_runtime import _forward_enqueue

        _forward_enqueue(
            "/api/v1/pb/UploadMel",
            pb_encode(
                **{
                    "1:str": nid,
                    "2:u64": ts,
                    "3:u32": MEL_BANDS,
                    "4:u32": MEL_FRAMES,
                    "5:bytes": mel_raw,
                    "6:f32": dmin,
                    "7:f32": dmax,
                }
            ),
            {
                "node_id": nid,
                "timestamp_ms": ts,
                "path_lora": True,
            },
        )
    return True
