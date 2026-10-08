"""HTTP mixin extracted from nevod_hub.Handler — Wave C."""
from __future__ import annotations

from typing import Any

class HubPbHttp:
    def _handle_pb_ingest(self, path: str) -> None:
        """Protobuf ingest. Ключ устройства не проверяем — его забирает облако."""
        assert STATE is not None
        body = self._read_body()
        if body is None:
            return
        device_bearer = _presented_bearer(self.headers.get("Authorization"))

        try:
            if path == "/api/v1/pb/ReportDetection":
                meta = validate_detection_pb(body)
                meta["bearer"] = device_bearer
                if _pb_is_pro(self.headers, str(meta.get("node_id") or "")):
                    self._json(403, {"error": "pro_not_on_hub", "accepted": False})
                    return
                STATE.mark(
                    "pb_detection", ok=True, meta=meta, transport="pb", body_len=len(body)
                )
                if FORWARDER is not None:
                    _forward_enqueue("/api/v1/pb/ReportDetection", body, meta)
                self._json(
                    200,
                    {
                        "accepted": True,
                        "schema": "nevod.detection.v1",
                        "event_id": str(uuid.uuid4()),
                    },
                )
            elif path == "/api/v1/pb/ReportHeartbeat":
                meta = validate_heartbeat_pb(body)
                meta["bearer"] = device_bearer
                if _pb_is_pro(self.headers, str(meta.get("node_id") or "")):
                    self._json(403, {"error": "pro_not_on_hub", "accepted": False})
                    return
                STATE.mark(
                    "pb_heartbeat", ok=True, meta=meta, transport="pb", body_len=len(body)
                )
                if FORWARDER is not None:
                    _forward_enqueue("/api/v1/pb/ReportHeartbeat", body, meta)
                self._json(
                    200,
                    {
                        "accepted": True,
                        "schema": "nevod.heartbeat.v1",
                        "event_id": str(uuid.uuid4()),
                    },
                )
            elif path == "/api/v1/pb/UploadMel":
                meta = validate_mel_pb(body)
                meta["bearer"] = device_bearer
                if _pb_is_pro(self.headers, str(meta.get("node_id") or "")):
                    self._json(403, {"error": "pro_not_on_hub", "accepted": False})
                    return
                payload = meta.pop("_data", b"")
                hdrs = {k: v for k, v in self.headers.items()}
                if (
                    MEL_SAVE
                    and payload
                    and not is_emulated_mel_source(hdrs, str(meta.get("node_id") or ""))
                ):
                    try:
                        cls = str(meta.get("class_name") or "").strip()
                        cid = meta.get("class_id")
                        if not cls:
                            cls = str(hdrs.get("X-Nevod-Class") or hdrs.get("x-nevod-class") or "").strip()
                        if cid in (None, 0, "0"):
                            try:
                                cid = int(hdrs.get("X-Nevod-Class-Id") or hdrs.get("x-nevod-class-id") or 0)
                            except (TypeError, ValueError):
                                cid = 0
                        if not cls:
                            # Fallback: last DET class for this node (≤90s).
                            nid = str(meta.get("node_id") or "")
                            node = STATE.nodes.get(nid) if STATE else None
                            det = (node.last_detection if node else None) or {}
                            age = None
                            if node and getattr(node, "last_detection_ts", None):
                                age = time.time() - float(node.last_detection_ts)
                            elif det.get("timestamp_ms") and meta.get("timestamp_ms"):
                                age = abs(float(meta["timestamp_ms"]) - float(det["timestamp_ms"])) / 1000.0
                            if det and (age is None or age <= 90.0):
                                cls = str(det.get("class_name") or "").strip()
                                if cid in (None, 0):
                                    try:
                                        cid = int(det.get("class_id") or 0)
                                    except (TypeError, ValueError):
                                        cid = 0
                        saved = save_mel_payload(
                            data=payload,
                            node_id=str(meta["node_id"]),
                            timestamp_ms=int(meta["timestamp_ms"]),
                            num_bands=int(meta["num_bands"]),
                            num_frames=int(meta["num_frames"]),
                            encoding=str(meta.get("encoding") or "uint8_minmax"),
                            data_min=meta.get("data_min"),
                            data_max=meta.get("data_max"),
                            data_encoding=meta.get("data_encoding"),
                            wire_payload_bytes=meta.get("wire_payload_bytes"),
                            class_name=cls or None,
                            class_id=int(cid) if cid not in (None, "") else None,
                        )
                        meta.update(saved)
                    except OSError as e:
                        meta["save_error"] = str(e)
                else:
                    meta["saved"] = False
                    meta["save_skip"] = "emulation_or_disabled"
                STATE.mark(
                    "pb_mel", ok=True, meta=meta, transport="pb", body_len=len(body)
                )
                if FORWARDER is not None:
                    _forward_enqueue("/api/v1/pb/UploadMel", body, meta)
                self._json(
                    200,
                    {
                        "accepted": True,
                        "schema": "nevod.mel.v1",
                        "event_id": str(uuid.uuid4()),
                        "saved_file": meta.get("saved_file"),
                    },
                )
            elif path == "/nodes/mel" or path.startswith("/nodes/"):
                # Legacy float Mel / JSON /nodes/* removed — Cloud data = PB only.
                self._json(
                    410,
                    {
                        "error": "gone",
                        "accepted": False,
                        "hint": "use /api/v1/pb/* protobuf",
                    },
                )
            else:
                self._json(404, {"error": "not_found", "accepted": False})
        except json.JSONDecodeError:
            self._json(400, {"error": "bad_json", "accepted": False})
        except ValueError as e:
            fail_key = {
                "/api/v1/pb/ReportDetection": "pb_detection",
                "/api/v1/pb/ReportHeartbeat": "pb_heartbeat",
                "/api/v1/pb/UploadMel": "pb_mel",
            }.get(path, "pb_detection")
            STATE.mark(fail_key, ok=False, error=str(e))
            self._json(400, {"error": str(e), "accepted": False})


