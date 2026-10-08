#!/usr/bin/env python3
"""Hub-side DET/Mel episode coalesce (ingest-time).

See docs/superpowers/specs/2026-08-28-hub-det-episode-coalesce-design.md
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

GAP_MS = 10_000


def _canon(node_id: str | None) -> str:
    s = (node_id or "").strip()
    if s.lower().startswith("nevod-"):
        s = s[6:]
    return s.strip()[:64]


def _ms_from_iso(ts: str | None, fallback_ms: int) -> int:
    if not ts:
        return fallback_ms
    try:
        from datetime import datetime, timezone

        t = datetime.fromisoformat(ts.replace("Z", "+00:00"))
        return int(t.timestamp() * 1000)
    except Exception:
        return fallback_ms


@dataclass
class Episode:
    episode_id: str
    node_id: str
    t_start_ms: int
    t_last_ms: int
    hop_count: int = 0
    mel_count: int = 0
    class_name: str | None = None
    classes_seen: list[str] = field(default_factory=list)
    threat: Any = None
    confidence: Any = None
    azimuth_deg: Any = None
    last_mel_file: str | None = None
    mel_files: list[str] = field(default_factory=list)
    vias: set[str] = field(default_factory=set)
    open: bool = True

    @property
    def via(self) -> str:
        if len(self.vias) >= 2:
            return "mixed"
        if "mqtt" in self.vias:
            return "mqtt"
        if "pb" in self.vias:
            return "pb"
        return next(iter(self.vias), "")

    def touch_class(self, name: str | None) -> None:
        if not name:
            return
        self.class_name = str(name)
        if self.class_name not in self.classes_seen:
            self.classes_seen.append(self.class_name)

    def add_mel_file(self, path: str | None) -> None:
        if not path:
            return
        p = str(path)
        self.last_mel_file = p
        if p not in self.mel_files:
            self.mel_files.append(p)
            del self.mel_files[:-12]

    def to_meta(self) -> dict[str, Any]:
        return {
            "episode_id": self.episode_id,
            "node_id": self.node_id,
            "t_start_ms": self.t_start_ms,
            "t_last_ms": self.t_last_ms,
            "hop_count": self.hop_count,
            "mel_count": self.mel_count,
            "class_name": self.class_name,
            "classes_seen": list(self.classes_seen),
            "threat": self.threat,
            "confidence": self.confidence,
            "p": self.confidence,
            "azimuth_deg": self.azimuth_deg,
            "last_mel_file": self.last_mel_file,
            "mel_files": list(self.mel_files),
            "saved_file": self.last_mel_file,
            "via": self.via,
            "open": self.open,
        }

    def to_public(self) -> dict[str, Any]:
        return self.to_meta()


@dataclass
class EpisodeRegistry:
    open_by_node: dict[str, Episode] = field(default_factory=dict)
    # Last closed per node (for Mel attach within gap).
    last_closed: dict[str, Episode] = field(default_factory=dict)

    def clear(self) -> None:
        self.open_by_node.clear()
        self.last_closed.clear()

    def close_stale(self, now_ms: int, gap_ms: int = GAP_MS) -> list[Episode]:
        closed: list[Episode] = []
        for nid, ep in list(self.open_by_node.items()):
            if now_ms - ep.t_last_ms > gap_ms:
                ep.open = False
                self.last_closed[nid] = ep
                del self.open_by_node[nid]
                closed.append(ep)
        return closed

    def active_for(self, node_id: str | None, now_ms: int, gap_ms: int = GAP_MS) -> Episode | None:
        nid = _canon(node_id)
        if not nid:
            return None
        self.close_stale(now_ms, gap_ms)
        return self.open_by_node.get(nid)


def _via_token(via: str) -> str:
    v = (via or "").lower()
    if "mqtt" in v:
        return "mqtt"
    return "pb"


def apply_detection(
    reg: EpisodeRegistry,
    *,
    node_id: str,
    now_ms: int,
    class_name: str | None = None,
    threat: Any = None,
    confidence: Any = None,
    azimuth_deg: Any = None,
    via: str = "pb",
    gap_ms: int = GAP_MS,
) -> tuple[Episode, str]:
    """Returns (episode, action) where action is 'update' | 'open'."""
    nid = _canon(node_id)
    if not nid:
        raise ValueError("empty_node_id")
    reg.close_stale(now_ms, gap_ms)
    ep = reg.open_by_node.get(nid)
    if ep is not None and now_ms - ep.t_last_ms <= gap_ms:
        ep.hop_count += 1
        ep.t_last_ms = now_ms
        ep.touch_class(class_name)
        if threat is not None:
            ep.threat = threat
        if confidence is not None:
            ep.confidence = confidence
        if azimuth_deg is not None:
            ep.azimuth_deg = azimuth_deg
        ep.vias.add(_via_token(via))
        return ep, "update"

    if ep is not None:
        ep.open = False
        reg.last_closed[nid] = ep
        del reg.open_by_node[nid]

    new = Episode(
        episode_id=f"{nid}:{now_ms}",
        node_id=nid,
        t_start_ms=now_ms,
        t_last_ms=now_ms,
        hop_count=1,
        vias={_via_token(via)},
    )
    new.touch_class(class_name)
    new.threat = threat
    new.confidence = confidence
    new.azimuth_deg = azimuth_deg
    reg.open_by_node[nid] = new
    return new, "open"


def apply_mel(
    reg: EpisodeRegistry,
    *,
    node_id: str,
    now_ms: int,
    saved_file: str | None = None,
    via: str = "pb",
    gap_ms: int = GAP_MS,
) -> tuple[Episode | None, str]:
    """Returns (episode|None, action) action: 'attach' | 'orphan'."""
    nid = _canon(node_id)
    if not nid:
        return None, "orphan"
    reg.close_stale(now_ms, gap_ms)
    ep = reg.open_by_node.get(nid)
    if ep is None:
        closed = reg.last_closed.get(nid)
        if closed is not None and now_ms - closed.t_last_ms <= gap_ms:
            ep = closed
    if ep is None:
        return None, "orphan"
    ep.mel_count += 1
    ep.t_last_ms = now_ms
    ep.add_mel_file(saved_file)
    ep.vias.add(_via_token(via))
    # Mel on closed-but-within-gap reopens for further hops.
    if not ep.open:
        ep.open = True
        reg.open_by_node[nid] = ep
    return ep, "attach"


def find_event_index(events: list[dict[str, Any]], episode_id: str) -> int:
    for i, ev in enumerate(events):
        meta = ev.get("meta") if isinstance(ev.get("meta"), dict) else {}
        if meta.get("episode_id") == episode_id:
            return i
        if ev.get("episode_id") == episode_id:
            return i
    return -1


def episode_event_dict(ep: Episode, *, ts_iso: str) -> dict[str, Any]:
    via = ep.via
    if via == "mqtt":
        transport = "mqtt"
    elif via == "mixed":
        transport = "pb"
    else:
        transport = "pb"
    return {
        "ts": ts_iso,
        "key": "episode",
        "ok": True,
        "error": "",
        "node_id": ep.node_id,
        "transport": transport,
        "kind": "det",
        "via": "episode",
        "via_label": "Эпизод" + (f" · {via.upper()}" if via else ""),
        "meta": ep.to_meta(),
        "episode_id": ep.episode_id,
    }
