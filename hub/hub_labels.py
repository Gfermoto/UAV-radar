"""Hub node label path helpers."""
from __future__ import annotations

from typing import Any

from hub_store import HubStore, normalize_node_label
from hub_pb import _canon_node_id

def parse_hub_node_label_path(path: str) -> str | None:
    """'/api/hub/nodes/{id}/label' → raw id (ещё не canon). Иначе None."""
    p = (path or "").split("?", 1)[0].rstrip("/")
    prefix = "/api/hub/nodes/"
    suffix = "/label"
    if not p.startswith(prefix) or not p.endswith(suffix):
        return None
    mid = p[len(prefix) : -len(suffix)]
    if not mid or "/" in mid:
        return None
    return mid


def apply_node_label(store: HubStore, raw_id: Any, raw_label: Any) -> dict[str, str]:
    nid = _canon_node_id(raw_id)
    if not nid:
        raise ValueError("invalid_node_id")
    label = normalize_node_label(raw_label)
    store.set_node_label(nid, label)
    return {"node_id": nid, "label": label}


def attach_node_labels(
    nodes: list[dict[str, Any]], labels: dict[str, str] | None
) -> None:
    labels = labels or {}
    for n in nodes:
        nid = _canon_node_id(n.get("node_id")) or str(n.get("node_id") or "")
        n["label"] = labels.get(nid, "") if nid else ""


