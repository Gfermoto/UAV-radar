#!/usr/bin/env python3
"""Operator GT for Hub Mel: meta.gt_label, labeled Mel+WAV zip, correction zip."""
from __future__ import annotations

import io
import json
import zipfile
from pathlib import Path
from typing import Any

import hub_mel_invert
from hub_mel_io import (
    MEL_DIR,
    clip_event_ms,
    clip_in_range,
)

GT_LABELS = frozenset({"background", "drone", "ice_uav", "jet_uav"})


class MelGtError(ValueError):
    """bad_filename | gt_invalid | object_required | missing_meta | no_gt | invert_failed."""

    def __init__(self, message: str, file: str | None = None) -> None:
        super().__init__(message)
        self.file = file


def normalize_gt_label(raw: Any) -> str | None:
    """None/"" → clear. Else wire id or MelGtError('gt_invalid')."""
    if raw is None:
        return None
    if isinstance(raw, str) and raw.strip() == "":
        return None
    if not isinstance(raw, str):
        raise MelGtError("gt_invalid")
    val = raw.strip()
    if val not in GT_LABELS:
        raise MelGtError("gt_invalid")
    return val


def _bin_name(fname: str) -> str:
    name = (fname or "").strip()
    if (
        not name
        or "/" in name
        or "\\" in name
        or ".." in name
        or not name.startswith("mel_")
    ):
        raise MelGtError("bad_filename")
    if name.endswith(".bin.meta.json"):
        name = name[: -len(".meta.json")]
    if not name.endswith(".bin"):
        raise MelGtError("bad_filename")
    return name


def set_mel_gt(
    fname: str,
    gt_label: Any,
    mel_dir: Path | None = None,
) -> dict[str, Any]:
    """Write or delete meta.gt_label. Does not touch class_name / bin / wav."""
    mel_dir = mel_dir or MEL_DIR
    bin_name = _bin_name(fname)
    bin_path = mel_dir / bin_name
    meta_path = Path(str(bin_path) + ".meta.json")
    if not bin_path.is_file():
        raise FileNotFoundError(bin_name)
    if not meta_path.is_file():
        raise MelGtError("missing_meta")
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise MelGtError("missing_meta") from exc
    if not isinstance(meta, dict):
        raise MelGtError("missing_meta")
    normalized = normalize_gt_label(gt_label)
    if normalized is None:
        meta.pop("gt_label", None)
    else:
        meta["gt_label"] = normalized
    tmp = Path(str(meta_path) + ".tmp")
    tmp.write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(meta_path)
    return {"status": "ok", "file": bin_name, "gt_label": normalized}


def _read_meta(bin_path: Path) -> dict[str, Any] | None:
    meta_p = Path(str(bin_path) + ".meta.json")
    if not meta_p.is_file():
        return None
    try:
        loaded = json.loads(meta_p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return loaded if isinstance(loaded, dict) else None


def build_mel_gt_archive_zip(
    mel_dir: Path | None = None,
    *,
    start_ms: int | None = None,
    end_ms: int | None = None,
) -> bytes:
    """Only clips with valid gt_label. Fail-closed on invert."""
    mel_dir = mel_dir or MEL_DIR
    ranged = start_ms is not None or end_ms is not None
    clips: list[tuple[Path, dict[str, Any], str]] = []
    if mel_dir.is_dir():
        for p in sorted(mel_dir.glob("mel_*.bin")):
            if not p.is_file():
                continue
            meta = _read_meta(p)
            if not meta:
                continue
            gt = meta.get("gt_label")
            if not isinstance(gt, str) or gt.strip() not in GT_LABELS:
                continue
            gt = gt.strip()
            if ranged:
                try:
                    mtime = p.stat().st_mtime
                except OSError:
                    continue
                if not clip_in_range(clip_event_ms(meta, mtime), start_ms, end_ms):
                    continue
            clips.append((p, meta, gt))
    if not clips:
        raise MelGtError("no_gt")
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        lines: list[str] = []
        for bin_path, meta, gt in clips:
            try:
                wav_bytes = hub_mel_invert.ensure_mel_wav(bin_path, meta)
            except Exception as exc:
                raise MelGtError("invert_failed", file=bin_path.name) from exc
            stem = bin_path.stem
            zf.write(bin_path, arcname=f"{gt}/{bin_path.name}")
            zf.writestr(f"{gt}/{stem}.wav", wav_bytes)
            meta_p = Path(str(bin_path) + ".meta.json")
            if meta_p.is_file():
                zf.write(meta_p, arcname=f"{gt}/{bin_path.name}.meta.json")
            lines.append(
                json.dumps(
                    {
                        "stem": stem,
                        "gt_label": gt,
                        "class_name": meta.get("class_name"),
                        "node_id": meta.get("node_id"),
                        "timestamp_ms": meta.get("timestamp_ms"),
                        "file": f"{gt}/{bin_path.name}",
                    },
                    ensure_ascii=False,
                )
            )
        zf.writestr("manifest.jsonl", "\n".join(lines) + ("\n" if lines else ""))
    return buf.getvalue()


def _canon_label(raw: Any) -> str | None:
    """ice_uav/jet_uav/drone/background, регистр не важен. Иначе None."""
    if not isinstance(raw, str):
        return None
    val = raw.strip().lower()
    if val in GT_LABELS:
        return val
    return None


def _model_class(raw: Any) -> str | None:
    token = _canon_label(raw)
    if token:
        return token
    if isinstance(raw, str) and raw.strip():
        return raw.strip()[:32]
    return None


def _meta_firmware(meta: dict[str, Any]) -> str | None:
    """Только поле, уже лежащее в meta. Версию узла не подставляем."""
    for key in ("firmware_version", "firmware"):
        raw = meta.get(key)
        if isinstance(raw, str) and raw.strip():
            return raw.strip()[:64]
    return None


def _meta_ts(meta: dict[str, Any]) -> int | None:
    raw = meta.get("timestamp_ms")
    if isinstance(raw, bool) or raw is None:
        return None
    if isinstance(raw, (int, float)) and raw == int(raw):
        return int(raw)
    return None


def _meta_node(meta: dict[str, Any]) -> str | None:
    raw = meta.get("node_id")
    if isinstance(raw, str) and raw.strip():
        return raw.strip()[:32]
    return None


def _pair_rows(counter: dict[tuple[str | None, str], int]) -> list[dict[str, Any]]:
    rows = [
        {"model_class": model, "gt_label": gt, "n": n}
        for (model, gt), n in counter.items()
    ]
    rows.sort(key=lambda row: (-int(row["n"]), str(row["model_class"] or ""), str(row["gt_label"])))
    return rows


def _bump(counter: dict[tuple[str | None, str], int], model: str | None, gt: str) -> None:
    key = (model, gt)
    counter[key] = counter.get(key, 0) + 1


def build_mel_corrections_zip(
    mel_dir: Path | None = None,
    *,
    facts_by_name: dict[str, dict[str, Any]] | None = None,
) -> bytes:
    """Zip клипов, где gt_label задан и не равен классу приёма.

    Папка = человеческая метка. class_name и файлы на диске не меняются.
    Тренер не вызывается. Пустой набор — пустой manifest и нулевые счётчики.
    """
    mel_dir = mel_dir or MEL_DIR
    facts = facts_by_name or {}
    clips: list[tuple[Path, dict[str, Any], str, str | None]] = []
    if mel_dir.is_dir():
        for p in sorted(mel_dir.glob("mel_*.bin")):
            if not p.is_file():
                continue
            meta = _read_meta(p)
            if not meta:
                continue
            gt = _canon_label(meta.get("gt_label"))
            if gt is None:
                continue
            model = _model_class(meta.get("class_name"))
            if _canon_label(meta.get("class_name")) == gt:
                continue
            clips.append((p, meta, gt, model))
    overall: dict[tuple[str | None, str], int] = {}
    by_node: dict[str, dict[tuple[str | None, str], int]] = {}
    by_fw: dict[str, dict[tuple[str | None, str], int]] = {}
    lines: list[str] = []
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for bin_path, meta, gt, model in clips:
            fact = facts.get(bin_path.name)
            if not isinstance(fact, dict):
                fact = {}
            node_id = _meta_node(meta)
            if node_id is None:
                raw_node = fact.get("node_id")
                if isinstance(raw_node, str) and raw_node.strip():
                    node_id = raw_node.strip()[:32]
            ts = _meta_ts(meta)
            if ts is None and fact:
                raw_ts = fact.get("ts_ms")
                if isinstance(raw_ts, (int, float)) and not isinstance(raw_ts, bool):
                    ts = int(raw_ts)
            firmware = _meta_firmware(meta)
            if fact:
                confidence = fact.get("confidence")
                if isinstance(confidence, bool) or not isinstance(confidence, (int, float)):
                    confidence = None
                azimuth = fact.get("azimuth_deg")
                if isinstance(azimuth, bool) or not isinstance(azimuth, (int, float)):
                    azimuth = None
                az_ok = fact.get("azimuth_valid")
                azimuth_valid: int | None = int(az_ok) if az_ok in (0, 1, True, False) else None
                if isinstance(az_ok, bool):
                    azimuth_valid = 1 if az_ok else 0
            else:
                confidence = None
                azimuth = None
                azimuth_valid = None
            rec = {
                "mel_file": bin_path.name,
                "node_id": node_id,
                "ts": ts,
                "firmware": firmware,
                "model_class": model,
                "gt_label": gt,
                "confidence": confidence,
                "azimuth_deg": azimuth,
                "azimuth_valid": azimuth_valid,
            }
            lines.append(json.dumps(rec, ensure_ascii=False))
            zf.write(bin_path, arcname=f"{gt}/{bin_path.name}")
            meta_p = Path(str(bin_path) + ".meta.json")
            if meta_p.is_file():
                zf.write(meta_p, arcname=f"{gt}/{bin_path.name}.meta.json")
            _bump(overall, model, gt)
            if node_id:
                _bump(by_node.setdefault(node_id, {}), model, gt)
            if firmware:
                _bump(by_fw.setdefault(firmware, {}), model, gt)
        zf.writestr("manifest.jsonl", "\n".join(lines) + ("\n" if lines else ""))
        summary = {
            "n": len(lines),
            "model_to_gt": _pair_rows(overall),
            "by_node_id": {nid: _pair_rows(cnt) for nid, cnt in sorted(by_node.items())},
            "by_firmware": {fw: _pair_rows(cnt) for fw, cnt in sorted(by_fw.items())},
        }
        zf.writestr(
            "summary.json",
            json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
        )
    return buf.getvalue()
