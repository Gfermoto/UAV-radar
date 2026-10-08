#!/usr/bin/env python3
"""Nevod Hub — SQLite persistence (nodes snapshot, ingest log, tokens, queue).

detection_facts — одна строка на MQTT/PB/LoRa детекцию. Байты Mel не пишем,
только имя уже сохранённого файла. ts_ms — UTC. Окно ночи:

  SELECT ts_ms, via, class_name, class_id, confidence, confirmed,
         early_warning, decision, azimuth_deg, azimuth_valid,
         doa_confidence, sigma_deg, n, heading_ref, mel_file
  FROM detection_facts
  WHERE node_id = '746E8C' AND ts_ms BETWEEN :since AND :until
  ORDER BY ts_ms;

Тот же срез: GET /api/hub/detections?node_id=746E8C&since_ms=&until_ms=
Хранение 14 суток. CREATE TABLE IF NOT EXISTS не трогает старые строки.
"""
from __future__ import annotations

import json
import sqlite3
import threading
import time
from pathlib import Path
from typing import Any

NODE_LABEL_MAX = 40
DETECTION_FACT_KEEP_MS = 14 * 24 * 3600 * 1000
_DET_VIAS = ("mqtt", "pb", "lora")


def _finite(val: Any) -> float | None:
    if isinstance(val, bool) or not isinstance(val, (int, float)):
        return None
    x = float(val)
    if x != x or x in (float("inf"), float("-inf")):
        return None
    return x


def _flag(val: Any) -> int | None:
    if isinstance(val, bool):
        return 1 if val else 0
    if isinstance(val, int) and val in (0, 1):
        return val
    return None


def _short(val: Any, n: int) -> str | None:
    if val is None:
        return None
    s = str(val).strip()
    if not s:
        return None
    return s[:n]


def _class_id(val: Any) -> int | None:
    if isinstance(val, bool) or val is None:
        return None
    if isinstance(val, int):
        return val
    if isinstance(val, float) and val == int(val):
        return int(val)
    if isinstance(val, str) and val.strip().lstrip("-").isdigit():
        return int(val.strip())
    return None


def _count(val: Any) -> int | None:
    if isinstance(val, bool) or val is None:
        return None
    if isinstance(val, int):
        return val
    if isinstance(val, float) and val == int(val):
        return int(val)
    return None


def _mel_name(val: Any) -> str | None:
    if not isinstance(val, str):
        return None
    name = Path(val).name.strip()
    if not name or name in {".", ".."}:
        return None
    return name[:128]


def _canon_nid(node_id: str) -> str:
    nid = str(node_id or "").strip()
    if nid.lower().startswith("nevod-"):
        nid = nid[6:]
    return nid[:16]


def _bearing(rec: dict[str, Any]) -> dict[str, Any]:
    br = rec.get("bearing")
    return br if isinstance(br, dict) else {}


def _fusion(rec: dict[str, Any]) -> dict[str, Any]:
    ext = rec.get("extensions")
    if not isinstance(ext, dict):
        return {}
    fus = ext.get("fusion")
    return fus if isinstance(fus, dict) else {}


def _detection_row(
    rec: dict[str, Any],
    *,
    node_id: str,
    via: str,
    ts_ms: int,
    mel_file: str | None,
) -> tuple[Any, ...]:
    br = _bearing(rec)
    fus = _fusion(rec)
    class_name = _short(rec.get("class_name") or rec.get("class"), 32)
    confirmed = _flag(rec.get("confirmed"))
    if confirmed is None and str(rec.get("alarm_tier") or "") == "confirmed":
        confirmed = 1
    early = _flag(rec.get("early_warning"))
    if early is None and str(rec.get("alarm_tier") or "") == "early":
        early = 1
    decision = _short(fus.get("decision") or fus.get("nn_raw_top") or rec.get("decision"), 32)
    az = _finite(rec.get("azimuth_deg"))
    if az is None:
        az = _finite(br.get("azimuth_deg"))
    if "azimuth_valid" in rec and rec.get("azimuth_valid") is not None:
        az_ok = 1 if rec.get("azimuth_valid") else 0
    elif az is not None:
        az_ok = 1
    else:
        az_ok = 0
    if az_ok and az is None:
        az = 0.0
    if not az_ok:
        az = None
    doa = _finite(rec.get("doa_confidence"))
    if doa is None:
        doa = _finite(br.get("confidence"))
    sigma = _finite(rec.get("sigma_deg"))
    if sigma is None:
        sigma = _finite(rec.get("doa_sigma_deg"))
    if sigma is None:
        sigma = _finite(br.get("sigma_deg"))
    n = _count(rec.get("n"))
    if n is None:
        n = _count(rec.get("doa_n"))
    if n is None:
        n = _count(br.get("n"))
    href = _short(rec.get("heading_ref") or br.get("heading_ref"), 16)
    conf = _finite(rec.get("confidence"))
    if conf is None:
        conf = _finite(rec.get("p"))
    return (
        node_id,
        ts_ms,
        via,
        class_name,
        _class_id(rec.get("class_id")),
        conf,
        confirmed,
        early,
        decision,
        az,
        az_ok,
        doa,
        sigma,
        n,
        href,
        _mel_name(mel_file),
    )


def _fact_dict(row: sqlite3.Row) -> dict[str, Any]:
    return {k: row[k] for k in row.keys()}


def normalize_node_label(raw: Any) -> str:
    """Strip; empty OK (clear). Reject non-str, C0 controls, >40 code points."""
    if raw is None:
        return ""
    if not isinstance(raw, str):
        raise ValueError("label_invalid")
    s = raw.strip()
    if any(ord(c) < 32 for c in s):
        raise ValueError("label_invalid")
    if len(s) > NODE_LABEL_MAX:
        raise ValueError("label_too_long")
    return s


class HubStore:
    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(str(self.path), check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._init()

    def _init(self) -> None:
        with self._lock:
            c = self._conn.cursor()
            c.executescript(
                """
                CREATE TABLE IF NOT EXISTS meta (
                  k TEXT PRIMARY KEY,
                  v TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS device_tokens (
                  node_id TEXT PRIMARY KEY,
                  token TEXT NOT NULL,
                  created_at REAL NOT NULL
                );
                CREATE TABLE IF NOT EXISTS ingest_log (
                  id INTEGER PRIMARY KEY AUTOINCREMENT,
                  t REAL NOT NULL,
                  kind TEXT NOT NULL,
                  node_id TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS nodes_json (
                  node_id TEXT PRIMARY KEY,
                  payload TEXT NOT NULL,
                  updated_at REAL NOT NULL
                );
                CREATE TABLE IF NOT EXISTS forward_queue (
                  id INTEGER PRIMARY KEY AUTOINCREMENT,
                  created_at REAL NOT NULL,
                  path TEXT NOT NULL,
                  body BLOB NOT NULL,
                  attempts INTEGER NOT NULL DEFAULT 0,
                  last_error TEXT NOT NULL DEFAULT '',
                  next_at REAL NOT NULL
                );
                CREATE TABLE IF NOT EXISTS webhook_deliveries (
                  id INTEGER PRIMARY KEY AUTOINCREMENT,
                  ts REAL NOT NULL,
                  channel TEXT NOT NULL,
                  node_id TEXT NOT NULL,
                  ok INTEGER NOT NULL,
                  detail TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS node_labels (
                  node_id TEXT PRIMARY KEY,
                  label TEXT NOT NULL,
                  updated_at REAL NOT NULL
                );
                CREATE TABLE IF NOT EXISTS detection_facts (
                  id INTEGER PRIMARY KEY AUTOINCREMENT,
                  node_id TEXT NOT NULL,
                  ts_ms INTEGER NOT NULL,
                  via TEXT NOT NULL,
                  class_name TEXT,
                  class_id INTEGER,
                  confidence REAL,
                  confirmed INTEGER,
                  early_warning INTEGER,
                  decision TEXT,
                  azimuth_deg REAL,
                  azimuth_valid INTEGER NOT NULL DEFAULT 0,
                  doa_confidence REAL,
                  sigma_deg REAL,
                  n INTEGER,
                  heading_ref TEXT,
                  mel_file TEXT
                );
                CREATE INDEX IF NOT EXISTS idx_ingest_t ON ingest_log(t);
                CREATE INDEX IF NOT EXISTS idx_fwd_next ON forward_queue(next_at);
                CREATE INDEX IF NOT EXISTS idx_det_facts_node_ts
                  ON detection_facts(node_id, ts_ms);
                CREATE INDEX IF NOT EXISTS idx_det_facts_ts ON detection_facts(ts_ms);
                """
            )
            self._conn.commit()
            cols = {
                str(r[1])
                for r in self._conn.execute("PRAGMA table_info(forward_queue)").fetchall()
            }
            if "bearer" not in cols:
                self._conn.execute(
                    "ALTER TABLE forward_queue ADD COLUMN bearer TEXT NOT NULL DEFAULT ''"
                )
                self._conn.commit()
            if "node_id" not in cols:
                self._conn.execute(
                    "ALTER TABLE forward_queue ADD COLUMN node_id TEXT NOT NULL DEFAULT ''"
                )
                self._conn.commit()

    def close(self) -> None:
        with self._lock:
            self._conn.close()

    def set_meta(self, k: str, v: Any) -> None:
        with self._lock:
            self._conn.execute(
                "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v",
                (k, json.dumps(v, ensure_ascii=False)),
            )
            self._conn.commit()

    def get_meta(self, k: str, default: Any = None) -> Any:
        with self._lock:
            row = self._conn.execute("SELECT v FROM meta WHERE k=?", (k,)).fetchone()
        if not row:
            return default
        try:
            return json.loads(row["v"])
        except json.JSONDecodeError:
            return default

    def set_device_token(self, node_id: str, token: str) -> None:
        with self._lock:
            self._conn.execute(
                "INSERT INTO device_tokens(node_id,token,created_at) VALUES(?,?,?) "
                "ON CONFLICT(node_id) DO UPDATE SET token=excluded.token",
                (node_id, token, time.time()),
            )
            self._conn.commit()

    def device_token(self, node_id: str) -> str | None:
        with self._lock:
            row = self._conn.execute(
                "SELECT token FROM device_tokens WHERE node_id=?", (node_id,)
            ).fetchone()
        return str(row["token"]) if row else None

    def all_device_tokens(self) -> dict[str, str]:
        with self._lock:
            rows = self._conn.execute("SELECT node_id, token FROM device_tokens").fetchall()
        return {str(r["node_id"]): str(r["token"]) for r in rows}

    def set_node_label(self, node_id: str, label: str) -> None:
        nid = str(node_id or "").strip()
        lab = str(label or "")
        with self._lock:
            if not lab:
                self._conn.execute("DELETE FROM node_labels WHERE node_id=?", (nid,))
            else:
                self._conn.execute(
                    "INSERT INTO node_labels(node_id,label,updated_at) VALUES(?,?,?) "
                    "ON CONFLICT(node_id) DO UPDATE SET "
                    "label=excluded.label, updated_at=excluded.updated_at",
                    (nid, lab, time.time()),
                )
            self._conn.commit()

    def node_label(self, node_id: str) -> str:
        nid = str(node_id or "").strip()
        with self._lock:
            row = self._conn.execute(
                "SELECT label FROM node_labels WHERE node_id=?", (nid,)
            ).fetchone()
        return str(row["label"]) if row else ""

    def all_node_labels(self) -> dict[str, str]:
        with self._lock:
            rows = self._conn.execute(
                "SELECT node_id, label FROM node_labels"
            ).fetchall()
        return {str(r["node_id"]): str(r["label"]) for r in rows}

    def append_ingest(self, kind: str, node_id: str, t: float | None = None) -> None:
        t = time.time() if t is None else float(t)
        with self._lock:
            self._conn.execute(
                "INSERT INTO ingest_log(t,kind,node_id) VALUES(?,?,?)",
                (t, kind, node_id),
            )
            # prune > 24h
            self._conn.execute("DELETE FROM ingest_log WHERE t < ?", (t - 86400,))
            self._conn.commit()

    def load_ingest(self, since_t: float) -> list[dict[str, Any]]:
        with self._lock:
            rows = self._conn.execute(
                "SELECT t, kind, node_id FROM ingest_log WHERE t >= ? ORDER BY t ASC",
                (since_t,),
            ).fetchall()
        return [{"t": float(r["t"]), "kind": r["kind"], "node_id": r["node_id"]} for r in rows]

    def append_detection(
        self,
        rec: dict[str, Any],
        *,
        via: str,
        node_id: str,
        ts_ms: int,
        mel_file: str | None = None,
    ) -> None:
        """Одна узкая строка детекции. ingest_log не меняется."""
        kind = via if via in _DET_VIAS else ""
        nid = _canon_nid(node_id)
        if not kind or not nid:
            return
        try:
            ts = int(ts_ms)
        except (TypeError, ValueError):
            return
        if ts < 1_000_000_000_000:
            return
        row = _detection_row(rec, node_id=nid, via=kind, ts_ms=ts, mel_file=mel_file)
        cutoff = int(time.time() * 1000) - DETECTION_FACT_KEEP_MS
        with self._lock:
            self._conn.execute(
                "INSERT INTO detection_facts("
                "node_id,ts_ms,via,class_name,class_id,confidence,confirmed,"
                "early_warning,decision,azimuth_deg,azimuth_valid,doa_confidence,"
                "sigma_deg,n,heading_ref,mel_file"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                row,
            )
            self._conn.execute("DELETE FROM detection_facts WHERE ts_ms < ?", (cutoff,))
            self._conn.commit()

    def attach_mel_file(self, node_id: str, ts_ms: int, saved_file: str) -> None:
        """Имя уже сохранённого Mel к ближайшей детекции ±90 с. Файл не копируем."""
        nid = _canon_nid(node_id)
        name = _mel_name(saved_file)
        if not nid or not name:
            return
        try:
            ts = int(ts_ms)
        except (TypeError, ValueError):
            return
        lo, hi = ts - 90_000, ts + 90_000
        with self._lock:
            hit = self._conn.execute(
                "SELECT id FROM detection_facts "
                "WHERE node_id=? AND mel_file IS NULL AND ts_ms BETWEEN ? AND ? "
                "ORDER BY ABS(ts_ms - ?) ASC LIMIT 1",
                (nid, lo, hi, ts),
            ).fetchone()
            if not hit:
                return
            self._conn.execute(
                "UPDATE detection_facts SET mel_file=? WHERE id=?",
                (name, int(hit["id"])),
            )
            self._conn.commit()

    def list_detections(
        self,
        *,
        node_id: str = "",
        since_ms: int = 0,
        until_ms: int | None = None,
        limit: int = 500,
    ) -> list[dict[str, Any]]:
        nid = _canon_nid(node_id) if node_id else ""
        try:
            since = int(since_ms)
        except (TypeError, ValueError):
            since = 0
        cap = max(1, min(int(limit), 2000))
        sql = (
            "SELECT id, node_id, ts_ms, via, class_name, class_id, confidence, "
            "confirmed, early_warning, decision, azimuth_deg, azimuth_valid, "
            "doa_confidence, sigma_deg, n, heading_ref, mel_file "
            "FROM detection_facts WHERE ts_ms>=?"
        )
        args: list[Any] = [since]
        if until_ms is not None:
            sql += " AND ts_ms<=?"
            args.append(int(until_ms))
        if nid:
            sql += " AND node_id=?"
            args.append(nid)
        sql += " ORDER BY ts_ms ASC LIMIT ?"
        args.append(cap)
        with self._lock:
            rows = self._conn.execute(sql, args).fetchall()
        return [_fact_dict(r) for r in rows]

    def facts_by_mel_names(self, names: list[str]) -> dict[str, dict[str, Any]]:
        """Последняя строка detection_facts на имя файла. Схему не меняет."""
        wanted: list[str] = []
        seen: set[str] = set()
        for raw in names:
            name = _mel_name(raw)
            if not name or name in seen:
                continue
            seen.add(name)
            wanted.append(name)
        if not wanted:
            return {}
        rows_all: list[sqlite3.Row] = []
        with self._lock:
            for i in range(0, len(wanted), 400):
                chunk = wanted[i : i + 400]
                marks = ",".join("?" for _ in chunk)
                rows_all.extend(
                    self._conn.execute(
                        "SELECT id, node_id, ts_ms, via, class_name, class_id, confidence, "
                        "confirmed, early_warning, decision, azimuth_deg, azimuth_valid, "
                        "doa_confidence, sigma_deg, n, heading_ref, mel_file "
                        "FROM detection_facts WHERE mel_file IN (" + marks + ") "
                        "ORDER BY ts_ms ASC, id ASC",
                        chunk,
                    ).fetchall()
                )
        found: dict[str, dict[str, Any]] = {}
        for row in rows_all:
            item = _fact_dict(row)
            key = item.get("mel_file")
            if isinstance(key, str) and key:
                found[key] = item
        return found

    def upsert_node(self, node_id: str, payload: dict[str, Any]) -> None:
        with self._lock:
            self._conn.execute(
                "INSERT INTO nodes_json(node_id,payload,updated_at) VALUES(?,?,?) "
                "ON CONFLICT(node_id) DO UPDATE SET payload=excluded.payload, updated_at=excluded.updated_at",
                (node_id, json.dumps(payload, ensure_ascii=False), time.time()),
            )
            self._conn.commit()

    def enqueue_forward(
        self,
        path: str,
        body: bytes,
        *,
        max_depth: int = 256,
        bearer: str = "",
        node_id: str = "",
    ) -> int:
        """Enqueue PB body for Cloud uplink. Drop oldest when over max_depth (backpressure)."""
        with self._lock:
            n = int(
                self._conn.execute("SELECT COUNT(*) AS c FROM forward_queue").fetchone()["c"]
            )
            if max_depth > 0 and n >= max_depth:
                overflow = n - max_depth + 1
                self._conn.execute(
                    "DELETE FROM forward_queue WHERE id IN ("
                    "SELECT id FROM forward_queue ORDER BY id ASC LIMIT ?)",
                    (overflow,),
                )
                row = self._conn.execute(
                    "SELECT v FROM meta WHERE k=?", ("forward_dropped",)
                ).fetchone()
                prev = 0
                if row:
                    try:
                        prev = int(json.loads(row["v"]))
                    except (TypeError, ValueError, json.JSONDecodeError):
                        prev = 0
                self._conn.execute(
                    "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v",
                    ("forward_dropped", json.dumps(prev + overflow)),
                )
            cur = self._conn.execute(
                "INSERT INTO forward_queue(created_at,path,body,next_at,bearer,node_id) "
                "VALUES(?,?,?,?,?,?)",
                (
                    time.time(),
                    path,
                    body,
                    time.time(),
                    (bearer or "").strip(),
                    (node_id or "").strip(),
                ),
            )
            self._conn.commit()
            return int(cur.lastrowid)

    def dequeue_due(self, limit: int = 20) -> list[dict[str, Any]]:
        now = time.time()
        with self._lock:
            rows = self._conn.execute(
                "SELECT id, path, body, attempts, bearer, created_at FROM forward_queue "
                "WHERE next_at <= ? ORDER BY id ASC LIMIT ?",
                (now, limit),
            ).fetchall()
        return [
            {
                "id": int(r["id"]),
                "path": r["path"],
                "body": bytes(r["body"]),
                "attempts": int(r["attempts"]),
                "bearer": str(r["bearer"] or ""),
                "created_at": float(r["created_at"] or 0),
            }
            for r in rows
        ]

    def _node_status_locked(self) -> dict[str, Any]:
        row = self._conn.execute(
            "SELECT v FROM meta WHERE k=?", ("forward_node_status",)
        ).fetchone()
        if not row:
            return {}
        try:
            raw = json.loads(row["v"])
        except (TypeError, ValueError, json.JSONDecodeError):
            return {}
        return raw if isinstance(raw, dict) else {}

    def _write_node_status_locked(self, nodes: dict[str, Any]) -> None:
        self._conn.execute(
            "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v",
            ("forward_node_status", json.dumps(nodes)),
        )

    def _note_node_result_locked(self, node_id: str, *, ok: bool, err: str) -> None:
        nid = (node_id or "").strip()
        if not nid:
            return
        nodes = self._node_status_locked()
        rec = nodes.get(nid)
        if not isinstance(rec, dict):
            rec = {"last_ok_at": None, "last_error": ""}
        if ok:
            rec["last_ok_at"] = time.time()
            rec["last_error"] = ""
        else:
            rec["last_error"] = (err or "")[:500]
        nodes[nid] = rec
        self._write_node_status_locked(nodes)
        leftover = ""
        for item in nodes.values():
            if isinstance(item, dict) and item.get("last_error"):
                leftover = str(item["last_error"])
        self._conn.execute(
            "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v",
            ("forward_last_abandoned", json.dumps(leftover)),
        )

    def _queue_node_id_locked(self, item_id: int) -> str:
        row = self._conn.execute(
            "SELECT node_id FROM forward_queue WHERE id=?", (item_id,)
        ).fetchone()
        if not row:
            return ""
        return str(row["node_id"] or "")

    def forward_ok(self, item_id: int) -> None:
        with self._lock:
            nid = self._queue_node_id_locked(item_id)
            self._conn.execute("DELETE FROM forward_queue WHERE id=?", (item_id,))
            if nid:
                self._note_node_result_locked(nid, ok=True, err="")
            else:
                self._conn.execute(
                    "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v",
                    ("forward_last_abandoned", json.dumps("")),
                )
            self._conn.commit()

    def forward_fail(self, item_id: int, err: str, *, backoff_s: float = 30.0) -> None:
        with self._lock:
            self._conn.execute(
                "UPDATE forward_queue SET attempts=attempts+1, last_error=?, next_at=? WHERE id=?",
                (err[:500], time.time() + backoff_s, item_id),
            )
            self._conn.commit()

    def abandon_forward(self, item_id: int, err: str) -> None:
        """Drop item after max attempts; keep last error in meta for UI."""
        with self._lock:
            nid = self._queue_node_id_locked(item_id)
            self._conn.execute("DELETE FROM forward_queue WHERE id=?", (item_id,))
            if nid:
                self._note_node_result_locked(nid, ok=False, err=err)
            else:
                self._conn.execute(
                    "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v",
                    ("forward_last_abandoned", json.dumps((err or "")[:500])),
                )
            row = self._conn.execute(
                "SELECT v FROM meta WHERE k=?", ("forward_abandoned",)
            ).fetchone()
            prev = 0
            if row:
                try:
                    prev = int(json.loads(row["v"]))
                except (TypeError, ValueError, json.JSONDecodeError):
                    prev = 0
            self._conn.execute(
                "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v",
                ("forward_abandoned", json.dumps(prev + 1)),
            )
            self._conn.commit()

    def forward_stats(self) -> dict[str, Any]:
        with self._lock:
            n = self._conn.execute("SELECT COUNT(*) AS c FROM forward_queue").fetchone()["c"]
            last = self._conn.execute(
                "SELECT last_error FROM forward_queue WHERE last_error != '' "
                "ORDER BY id DESC LIMIT 1"
            ).fetchone()
            dropped_row = self._conn.execute(
                "SELECT v FROM meta WHERE k=?", ("forward_dropped",)
            ).fetchone()
            abandoned_row = self._conn.execute(
                "SELECT v FROM meta WHERE k=?", ("forward_abandoned",)
            ).fetchone()
            abandoned_err = self._conn.execute(
                "SELECT v FROM meta WHERE k=?", ("forward_last_abandoned",)
            ).fetchone()
            node_status = self._node_status_locked()

        def _meta_int(row: Any) -> int:
            if not row:
                return 0
            try:
                return int(json.loads(row["v"]))
            except (TypeError, ValueError, json.JSONDecodeError):
                return 0

        def _meta_str(row: Any) -> str:
            if not row:
                return ""
            try:
                v = json.loads(row["v"])
                return str(v) if v is not None else ""
            except (TypeError, ValueError, json.JSONDecodeError):
                return ""

        last_err = (last["last_error"] if last else "") or _meta_str(abandoned_err)
        return {
            "queue_depth": int(n),
            "last_error": last_err,
            "dropped_total": _meta_int(dropped_row),
            "abandoned_total": _meta_int(abandoned_row),
            "nodes": node_status,
        }

    def log_webhook(self, channel: str, node_id: str, ok: bool, detail: str) -> None:
        with self._lock:
            self._conn.execute(
                "INSERT INTO webhook_deliveries(ts,channel,node_id,ok,detail) VALUES(?,?,?,?,?)",
                (time.time(), channel, node_id, 1 if ok else 0, detail[:500]),
            )
            self._conn.execute(
                "DELETE FROM webhook_deliveries WHERE id NOT IN ("
                "SELECT id FROM webhook_deliveries ORDER BY id DESC LIMIT 200)"
            )
            self._conn.commit()

    def recent_webhooks(self, limit: int = 40) -> list[dict[str, Any]]:
        with self._lock:
            rows = self._conn.execute(
                "SELECT ts, channel, node_id, ok, detail FROM webhook_deliveries "
                "ORDER BY id DESC LIMIT ?",
                (limit,),
            ).fetchall()
        return [
            {
                "ts": float(r["ts"]),
                "channel": r["channel"],
                "node_id": r["node_id"],
                "ok": bool(r["ok"]),
                "detail": r["detail"],
            }
            for r in rows
        ]
