#!/usr/bin/env python3
"""Mel disk I/O, analytics, quota for Nevod Hub."""
from __future__ import annotations

import io
import json
import math
import os
import re
import struct
import tempfile
import time
import zipfile
import zlib
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

MEL_FMIN = 125
MEL_FMAX = 7500
MEL_BANDS_EXPECTED = 64
MEL_FRAMES_MAX = 401
MEL_DATA_MAX = 26112  # nanopb MelFrame.data
MIN_UNIX_TS_MS = 1_000_000_000_000
# Flat Mel: argmax на краю сетки — артефакт, не тон. peak только при выраженном горбе.
MEL_PEAK_MIN_PEAKINESS = 1.40  # max/median band_means
MEL_PEAK_MAX_FLATNESS = 0.75  # geo/arith mean; выше → broadband
# Europe/Moscow: UTC+3 year-round since 2014 (no tzdata in slim images).
MEL_CALENDAR_TZ = timezone(timedelta(hours=3))
_YMD_RE = re.compile(r"^(\d{4})-(\d{2})-(\d{2})$")


def _spectrum_peak_from_means(band_means: list[float]) -> dict[str, Any]:
    """Peak band/Hz only when spectrum has a trusted bump; else peak_valid=false.

    На почти равных band_means raw argmax часто уезжает на край (в т.ч. верхнюю
    mel-полосу ~7.4 kHz) — ложный «HF/jet» в UI. Край сетки доверяем только при
    сильном не-плоском горбе; иначе shape=flat|lf_broadband|hf_broadband.
    """
    bands = len(band_means)
    empty = {
        "peak_band": None,
        "peak_hz_approx": None,
        "peak_valid": False,
        "peakiness": None,
        "flatness": None,
        "spectrum_shape": "empty",
    }
    if bands <= 0:
        return empty
    vals = [float(x) for x in band_means]
    peak_i = max(range(bands), key=lambda i: vals[i])
    lo, hi = bands // 4, max(bands // 4 + 1, 3 * bands // 4)
    mid = vals[lo:hi] or vals
    med = sorted(mid)[len(mid) // 2]
    peakiness = float(vals[peak_i] / (med + 1e-9))
    clipped = [max(v, 1e-6) for v in vals]
    am = sum(clipped) / len(clipped)
    gm = math.exp(sum(math.log(v) for v in clipped) / len(clipped))
    flatness = float(gm / (am + 1e-30))
    flatness = min(1.0, max(0.0, flatness))
    third = max(1, bands // 3)
    low_e = sum(vals[:third]) / third
    high_e = sum(vals[-third:]) / third
    hl = high_e / (low_e + 1e-9)

    def _invalid(shape: str) -> dict[str, Any]:
        return {
            "peak_band": None,
            "peak_hz_approx": None,
            "peak_valid": False,
            "peakiness": round(peakiness, 3),
            "flatness": round(flatness, 3),
            "spectrum_shape": shape,
        }

    if hl < 0.75:
        broadband_shape = "lf_broadband"
    elif hl > 1.25:
        broadband_shape = "hf_broadband"
    else:
        broadband_shape = "flat"

    edge = peak_i <= 1 or peak_i >= bands - 2
    # Слабый горб — не peak.
    if peakiness < MEL_PEAK_MIN_PEAKINESS:
        return _invalid(broadband_shape)
    # Край сетки + плоский фон → артефакт argmax (типичный false 7.4 kHz).
    if edge and flatness >= MEL_PEAK_MAX_FLATNESS:
        return _invalid(broadband_shape)
    # Край + умеренная плоскость: требуем более сильный горб.
    if edge and peakiness < 2.0:
        return _invalid(broadband_shape)

    f_peak = MEL_FMIN + (MEL_FMAX - MEL_FMIN) * (peak_i + 0.5) / bands
    return {
        "peak_band": peak_i,
        "peak_hz_approx": round(f_peak, 1),
        "peak_valid": True,
        "peakiness": round(peakiness, 3),
        "flatness": round(flatness, 3),
        "spectrum_shape": "tonal",
    }


class MelDateError(ValueError):
    """Query from/to: bad_date | bad_range."""


def parse_mel_date_range(
    from_s: str | None, to_s: str | None
) -> tuple[int | None, int | None]:
    """Inclusive calendar days in MEL_CALENDAR_TZ → (start_ms, end_ms)."""
    from_s = (from_s or "").strip() or None
    to_s = (to_s or "").strip() or None
    start_ms: int | None = None
    end_ms: int | None = None
    if from_s:
        start_ms = int(_parse_ymd(from_s).timestamp() * 1000)
    if to_s:
        end_dt = _parse_ymd(to_s) + timedelta(days=1) - timedelta(milliseconds=1)
        end_ms = int(end_dt.timestamp() * 1000)
    if start_ms is not None and end_ms is not None and start_ms > end_ms:
        raise MelDateError("bad_range")
    return start_ms, end_ms


def _parse_ymd(s: str) -> datetime:
    m = _YMD_RE.fullmatch(s)
    if not m:
        raise MelDateError("bad_date")
    try:
        return datetime(int(m.group(1)), int(m.group(2)), int(m.group(3)), tzinfo=MEL_CALENDAR_TZ)
    except ValueError as exc:
        raise MelDateError("bad_date") from exc


def clip_event_ms(meta: dict[str, Any] | None, mtime: float) -> int:
    """NTP timestamp_ms if wall-clock; else Hub file mtime (seconds → ms)."""
    ts = (meta or {}).get("timestamp_ms")
    if isinstance(ts, (int, float)) and ts >= MIN_UNIX_TS_MS:
        return int(ts)
    return int(float(mtime) * 1000)


def clip_in_range(event_ms: int, start_ms: int | None, end_ms: int | None) -> bool:
    if start_ms is not None and event_ms < start_ms:
        return False
    if end_ms is not None and event_ms > end_ms:
        return False
    return True


def mel_archive_download_name(
    kind: str, from_s: str | None, to_s: str | None
) -> str:
    """kind is MEL or MEL-gt."""
    from_s = (from_s or "").strip() or None
    to_s = (to_s or "").strip() or None
    if not from_s and not to_s:
        return f"{kind}.zip"
    a = (from_s or "start").replace("-", "")
    b = (to_s or "end").replace("-", "")
    return f"{kind}-{a}-{b}.zip"

MEL_DIR = Path(
    os.environ.get(
        "MEL_DIR",
        "/data/mel" if os.path.isdir("/data") else str(Path(tempfile.gettempdir()) / "nevod_mock_mel"),
    )
)
MEL_SAVE = os.environ.get("MEL_SAVE", "1").lower() in ("1", "true", "yes")
MEL_MAX_FILES = int(os.environ.get("MEL_MAX_FILES", "500"))
MEL_SKIP_NODES = {
    x.strip()
    for x in os.environ.get("MEL_SKIP_NODES", "").split(",")
    if x.strip()
}
_DEMO_NODE_RE = re.compile(r"DEMO", re.I)
_NODE_FILE_RE = re.compile(r"[^A-Za-z0-9._-]+")

def mel_analytics_uint8(
    data: bytes, bands: int, frames: int, dmin: float, dmax: float
) -> dict[str, Any]:
    """Band means (dequantized), global stats, coarse histogram — без хранения сырого MEL."""
    span = float(dmax) - float(dmin)
    scale = span / 255.0 if span > 0 else 0.0
    band_means = [0.0] * bands
    hist = [0] * 16
    vmin = float("inf")
    vmax = float("-inf")
    acc = 0.0
    n = bands * frames
    for i, b in enumerate(data):
        v = float(dmin) + b * scale
        acc += v
        if v < vmin:
            vmin = v
        if v > vmax:
            vmax = v
        band_means[i % bands] += v
        hist[min(15, b >> 4)] += 1
    if frames > 0:
        band_means = [x / frames for x in band_means]
    peak = _spectrum_peak_from_means(band_means)
    return {
        "band_means": [round(x, 4) for x in band_means],
        "value_min": round(vmin, 4) if n else None,
        "value_max": round(vmax, 4) if n else None,
        "value_mean": round(acc / n, 4) if n else None,
        "hist16": hist,
        "peak_band": peak["peak_band"],
        "peak_hz_approx": peak["peak_hz_approx"],
        "peak_valid": peak["peak_valid"],
        "peakiness": peak["peakiness"],
        "flatness": peak["flatness"],
        "spectrum_shape": peak["spectrum_shape"],
        "dynamic_range_db": round(span, 3),
    }


def mel_analytics_float32(raw: bytes, bands: int, frames: int) -> dict[str, Any]:
    n = bands * frames
    vals = struct.unpack(f"<{n}f", raw)
    band_means = [0.0] * bands
    vmin = float("inf")
    vmax = float("-inf")
    acc = 0.0
    for i, v in enumerate(vals):
        if not math.isfinite(v):
            continue
        acc += v
        if v < vmin:
            vmin = v
        if v > vmax:
            vmax = v
        band_means[i % bands] += v
    if frames > 0:
        band_means = [x / frames for x in band_means]
    peak = _spectrum_peak_from_means(band_means)
    return {
        "band_means": [round(x, 4) for x in band_means],
        "value_min": round(vmin, 4) if n else None,
        "value_max": round(vmax, 4) if n else None,
        "value_mean": round(acc / n, 4) if n else None,
        "peak_band": peak["peak_band"],
        "peak_hz_approx": peak["peak_hz_approx"],
        "peak_valid": peak["peak_valid"],
        "peakiness": peak["peakiness"],
        "flatness": peak["flatness"],
        "spectrum_shape": peak["spectrum_shape"],
    }


def is_emulated_mel_source(headers: dict[str, str], node_id: str) -> bool:
    """True → не писать MEL на диск (demo feeder / DEMO node_id)."""
    src = (
        headers.get("X-Nevod-Source")
        or headers.get("x-nevod-source")
        or ""
    ).strip().lower()
    if src in ("demo", "demo-feeder", "feeder", "emulation", "emu"):
        return True
    if node_id in MEL_SKIP_NODES:
        return True
    if _DEMO_NODE_RE.search(node_id or ""):
        return True
    return False


def _safe_node_file(node_id: str) -> str:
    s = _NODE_FILE_RE.sub("_", node_id or "unknown")
    return (s[:64] or "unknown")


def _enforce_mel_quota(mel_dir: Path, max_files: int = MEL_MAX_FILES) -> None:
    max_age = float(os.environ.get("HUB_MEL_MAX_AGE_S", "0") or 0)
    if max_age > 0 and mel_dir.is_dir():
        cutoff = time.time() - max_age
        for pth in list(mel_dir.glob("mel_*.bin")):
            try:
                if pth.stat().st_mtime < cutoff:
                    _unlink_mel_bin(pth)
            except OSError:
                pass
    if max_files <= 0 or not mel_dir.is_dir():
        return
    files = sorted(
        [p for p in mel_dir.glob("mel_*.bin") if p.is_file()],
        key=lambda p: p.stat().st_mtime,
    )
    while len(files) >= max_files:
        old = files.pop(0)
        try:
            _unlink_mel_bin(old)
        except OSError:
            break


def _unlink_mel_bin(bin_path: Path) -> None:
    """mel_*.bin + .meta.json + кэш Griffin-Lim .wav."""
    bin_path.unlink(missing_ok=True)
    Path(str(bin_path) + ".meta.json").unlink(missing_ok=True)
    bin_path.with_suffix(".wav").unlink(missing_ok=True)


def _safe_mel_label(label: str | None) -> str:
    """Sanitize UAV class for filename: drone/ice_uav/jet_uav/UAV."""
    if not label:
        return ""
    s = "".join(c if (c.isalnum() or c in "_-") else "_" for c in str(label).strip())
    s = s.strip("_")[:24]
    return s


def save_mel_payload(
    *,
    data: bytes,
    node_id: str,
    timestamp_ms: int,
    num_bands: int,
    num_frames: int,
    encoding: str,
    data_min: float | None = None,
    data_max: float | None = None,
    fmin: str | None = None,
    fmax: str | None = None,
    data_encoding: int | None = None,
    wire_payload_bytes: int | None = None,
    class_name: str | None = None,
    class_id: int | None = None,
    mel_dir: Path | None = None,
) -> dict[str, Any]:
    """Пишет mel_{node}_{label}_{ts}.bin + .meta.json. Returns paths/meta.

    data — всегда сырой uint8[bands*frames] (после gunzip на приёме).
    """
    mel_dir = mel_dir or MEL_DIR
    mel_dir.mkdir(parents=True, exist_ok=True)
    _enforce_mel_quota(mel_dir)
    safe = _safe_node_file(node_id)
    ts = int(timestamp_ms) if timestamp_ms else int(time.time() * 1000)
    label = _safe_mel_label(class_name)
    if label:
        fname = f"mel_{safe}_{label}_{ts}.bin"
    else:
        fname = f"mel_{safe}_{ts}.bin"
    path = mel_dir / fname
    path.write_bytes(data)
    meta = {
        "encoding": encoding,
        "node_id": node_id,
        "timestamp_ms": ts,
        "num_bands": int(num_bands),
        "num_frames": int(num_frames),
        "payload_bytes": len(data),
        "file": fname,
    }
    if label:
        meta["class_name"] = label
    if class_id is not None:
        meta["class_id"] = int(class_id)
    if data_min is not None:
        meta["data_min"] = float(data_min)
    if data_max is not None:
        meta["data_max"] = float(data_max)
    if fmin is not None:
        meta["fmin"] = fmin
    if fmax is not None:
        meta["fmax"] = fmax
    if data_encoding is not None:
        meta["data_encoding"] = int(data_encoding)
    if wire_payload_bytes is not None:
        meta["wire_payload_bytes"] = int(wire_payload_bytes)
    meta_path = Path(str(path) + ".meta.json")
    meta_path.write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    return {"saved_file": fname, "saved_path": str(path), "saved_meta": str(meta_path.name)}


def list_saved_mel(
    mel_dir: Path | None = None,
    limit: int = 50,
    *,
    start_ms: int | None = None,
    end_ms: int | None = None,
    node_id: str | None = None,
    sort: str = "date",
) -> list[dict[str, Any]]:
    mel_dir = mel_dir or MEL_DIR
    if not mel_dir.is_dir():
        return []
    files = sorted(
        [p for p in mel_dir.glob("mel_*.bin") if p.is_file()],
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    cap = max(1, int(limit))
    ranged = start_ms is not None or end_ms is not None
    out: list[dict[str, Any]] = []
    for p in files:
        st = p.stat()
        meta: dict[str, Any] = {}
        meta_p = Path(str(p) + ".meta.json")
        if meta_p.is_file():
            try:
                loaded = json.loads(meta_p.read_text(encoding="utf-8"))
                if isinstance(loaded, dict):
                    meta = loaded
            except (OSError, json.JSONDecodeError):
                pass
        if ranged and not clip_in_range(clip_event_ms(meta, st.st_mtime), start_ms, end_ms):
            continue
        if node_id:
            needle = node_id.strip().upper().replace("-", "").replace("NEVOD", "")
            hay = (
                str((meta.get("node_id") if meta else "") or "")
                + " "
                + p.name
            ).upper().replace("-", "")
            if needle and needle not in hay:
                continue
        mtime_dt = datetime.fromtimestamp(st.st_mtime, timezone.utc)
        item: dict[str, Any] = {
            "file": p.name,
            "size": st.st_size,
            "mtime": mtime_dt.isoformat(),
            "received_at": mtime_dt.isoformat(),
            "url": f"/api/mel/{p.name}",
        }
        if meta:
            item["meta"] = meta
        ts_ms = meta.get("timestamp_ms")
        if isinstance(ts_ms, (int, float)) and ts_ms >= MIN_UNIX_TS_MS:
            item["captured_at"] = datetime.fromtimestamp(
                float(ts_ms) / 1000.0, timezone.utc
            ).isoformat()
            item["clock_ok"] = True
        elif isinstance(ts_ms, (int, float)) and ts_ms > 0:
            item["clock_ok"] = False
            item["capture_note"] = "нет NTP (uptime)"
        out.append(item)
        if len(out) >= cap:
            break
    if str(sort or "date").strip().lower() == "node":
        out.sort(key=lambda it: str(it.get("received_at") or ""), reverse=True)
        out.sort(key=lambda it: str((it.get("meta") or {}).get("node_id") or "").upper())
    return out



def mel_heat_rgb(t: float) -> tuple[int, int, int]:
    """WebUI-matching blue→cyan→yellow→red (hub_mel.js melHeatRgb)."""
    t = max(0.0, min(1.0, float(t)))
    r = g = b = 0
    if t < 0.25:
        u = t / 0.25
        b = int(80 + u * 175)
    elif t < 0.5:
        u = (t - 0.25) / 0.25
        g = int(u * 220)
        b = 255
    elif t < 0.75:
        u = (t - 0.5) / 0.25
        r = int(u * 255)
        g = int(220 - u * 40)
    else:
        u = (t - 0.75) / 0.25
        r = 255
        g = int(180 - u * 180)
    return r, g, b


def mel_visual_scale(grid: list[list[float]]) -> tuple[float, float]:
    """Display-only p5–p95 (same as hub_mel.js melVisualScale)."""
    vals: list[float] = []
    for row in grid:
        for v in row:
            if math.isfinite(v):
                vals.append(float(v))
    if not vals:
        return 0.0, 1.0
    vals.sort()
    n = len(vals)

    def at(q: float) -> float:
        return vals[max(0, min(n - 1, int(q * (n - 1))))]

    dmin, dmax = at(0.05), at(0.95)
    if not (dmax > dmin):
        dmin, dmax = vals[0], vals[-1]
    if not (dmax > dmin):
        dmax = dmin + 1e-3
    return dmin, dmax


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(tag)
    crc = zlib.crc32(data, crc) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)


def encode_png_rgb(width: int, height: int, rgb: bytes) -> bytes:
    """Minimal RGB8 PNG (no deps). rgb = row-major top→bottom, 3 bytes/pixel."""
    if width <= 0 or height <= 0:
        raise ValueError("bad_png_dims")
    need = width * height * 3
    if len(rgb) < need:
        raise ValueError("short_rgb")
    row = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter None
        raw.extend(rgb[y * row : (y + 1) * row])
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + _png_chunk(b"IEND", b"")
    )


def render_mel_png(
    data: bytes,
    bands: int,
    frames: int,
    dmin: float,
    dmax: float,
    *,
    scale: int = 3,
    use_visual_scale: bool = True,
) -> bytes:
    """Decode uint8 Mel → heat PNG. Band 0 at bottom (low freq), like WebUI."""
    if bands <= 0 or frames <= 0 or scale < 1:
        raise ValueError("bad_mel_dims")
    need = bands * frames
    if len(data) < need:
        raise ValueError("short_mel")
    grid = decode_mel_uint8(data[:need], bands, frames, dmin, dmax)
    if use_visual_scale:
        vmin, vmax = mel_visual_scale(grid)
    else:
        vmin, vmax = float(dmin), float(dmax)
        if not (vmax > vmin):
            vmax = vmin + 1e-3
    span = vmax - vmin
    w, h = frames * scale, bands * scale
    rgb = bytearray(w * h * 3)
    for f in range(frames):
        for b in range(bands):
            t = (grid[f][b] - vmin) / span
            r, g, bl = mel_heat_rgb(t)
            # band 0 → bottom
            y0 = (bands - 1 - b) * scale
            x0 = f * scale
            for dy in range(scale):
                for dx in range(scale):
                    i = ((y0 + dy) * w + (x0 + dx)) * 3
                    rgb[i] = r
                    rgb[i + 1] = g
                    rgb[i + 2] = bl
    return encode_png_rgb(w, h, bytes(rgb))


def _mel_png_for_bin(bin_path: Path) -> bytes | None:
    """Build PNG for one mel_*.bin using companion meta when present."""
    try:
        data = bin_path.read_bytes()
    except OSError:
        return None
    bands = MEL_BANDS_EXPECTED
    frames = 0
    dmin, dmax = 0.0, 255.0
    meta_p = Path(str(bin_path) + ".meta.json")
    if meta_p.is_file():
        try:
            meta = json.loads(meta_p.read_text(encoding="utf-8"))
            bands = int(meta.get("num_bands") or bands)
            frames = int(meta.get("num_frames") or 0)
            if meta.get("data_min") is not None and meta.get("data_max") is not None:
                dmin = float(meta["data_min"])
                dmax = float(meta["data_max"])
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            pass
    if frames <= 0 and bands > 0:
        frames = len(data) // bands
    if bands <= 0 or frames <= 0 or len(data) < bands * frames:
        return None
    try:
        return render_mel_png(data, bands, frames, dmin, dmax)
    except ValueError:
        return None


def build_mel_archive_zip(
    mel_dir: Path | None = None,
    *,
    start_ms: int | None = None,
    end_ms: int | None = None,
) -> bytes:
    """Zip mel_*.bin + .meta.json + decoded heat PNG (same palette as WebUI)."""
    mel_dir = mel_dir or MEL_DIR
    ranged = start_ms is not None or end_ms is not None
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        if mel_dir.is_dir():
            for p in sorted(mel_dir.glob("mel_*.bin")):
                if ranged:
                    meta: dict[str, Any] = {}
                    meta_p = Path(str(p) + ".meta.json")
                    if meta_p.is_file():
                        try:
                            loaded = json.loads(meta_p.read_text(encoding="utf-8"))
                            if isinstance(loaded, dict):
                                meta = loaded
                        except (OSError, json.JSONDecodeError):
                            pass
                    try:
                        mtime = p.stat().st_mtime
                    except OSError:
                        continue
                    if not clip_in_range(clip_event_ms(meta, mtime), start_ms, end_ms):
                        continue
                try:
                    zf.write(p, arcname=p.name)
                except OSError as exc:
                    raise OSError(f"mel_zip_read:{p.name}:{exc}") from exc
                meta_path = Path(str(p) + ".meta.json")
                if meta_path.is_file():
                    try:
                        zf.write(meta_path, arcname=meta_path.name)
                    except OSError:
                        pass
                png = _mel_png_for_bin(p)
                if png:
                    zf.writestr(p.stem + ".png", png)
    return buf.getvalue()

def clear_saved_mel(mel_dir: Path | None = None) -> dict[str, int]:
    """Удаляет mel_*.bin (+ .meta.json). Не трогает mqtt_mirror.json и прочее."""
    mel_dir = mel_dir or MEL_DIR
    removed = 0
    if not mel_dir.is_dir():
        return {"removed": 0}
    for p in list(mel_dir.glob("mel_*.bin")):
        try:
            _unlink_mel_bin(p)
            removed += 1
        except OSError:
            pass
    for wp in list(mel_dir.glob("mel_*.wav")):
        try:
            wp.unlink(missing_ok=True)
        except OSError:
            pass
    # хвостовые meta без bin
    for mp in list(mel_dir.glob("mel_*.bin.meta.json")):
        try:
            mp.unlink(missing_ok=True)
        except OSError:
            pass
    return {"removed": removed}


def delete_saved_mel(fname: str, mel_dir: Path | None = None) -> dict[str, Any]:
    """Удаляет один mel_*.bin (+ companion .meta.json)."""
    mel_dir = mel_dir or MEL_DIR
    name = (fname or "").strip()
    if (
        not name
        or "/" in name
        or "\\" in name
        or ".." in name
        or not name.startswith("mel_")
    ):
        raise ValueError("bad_filename")
    if name.endswith(".bin.meta.json"):
        bin_name = name[: -len(".meta.json")]
    elif name.endswith(".bin"):
        bin_name = name
    else:
        raise ValueError("bad_filename")
    if not bin_name.startswith("mel_") or not bin_name.endswith(".bin"):
        raise ValueError("bad_filename")
    bin_path = mel_dir / bin_name
    meta_path = Path(str(bin_path) + ".meta.json")
    if not bin_path.is_file() and not meta_path.is_file():
        raise FileNotFoundError(bin_name)
    removed_bin = False
    removed_meta = False
    wav_path = bin_path.with_suffix(".wav")
    removed_wav = False
    if bin_path.is_file():
        bin_path.unlink()
        removed_bin = True
    if meta_path.is_file():
        meta_path.unlink()
        removed_meta = True
    if wav_path.is_file():
        wav_path.unlink()
        removed_wav = True
    rev_path = Path(str(wav_path) + ".rev")
    if rev_path.is_file():
        rev_path.unlink()
    return {
        "removed": 1 if (removed_bin or removed_meta or removed_wav) else 0,
        "file": bin_name,
        "removed_bin": removed_bin,
        "removed_meta": removed_meta,
        "removed_wav": removed_wav,
    }


def decode_mel_uint8(
    data: bytes, bands: int, frames: int, dmin: float, dmax: float
) -> list[list[float]]:
    """Restore float Mel grid from uint8 minmax (for self-test / docs)."""
    need = bands * frames
    if len(data) < need or bands <= 0 or frames <= 0:
        raise ValueError("bad_mel_dims")
    span = float(dmax) - float(dmin)
    out: list[list[float]] = []
    for f in range(frames):
        row: list[float] = []
        base = f * bands
        for b in range(bands):
            u = data[base + b]
            row.append(float(dmin) + (u / 255.0) * span)
        out.append(row)
    return out
