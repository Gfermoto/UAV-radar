"""HTTP mixin extracted from nevod_hub.Handler — Wave C."""
from __future__ import annotations

from typing import Any

class HubMelHttp:
    def _handle_mel_post(self, path: str) -> bool:
        """Mel clear/delete/gt POSTs. Returns True if path handled."""
        assert STATE is not None
        if path.startswith("/api/mel/") and path.endswith("/gt"):
            if not self._auth_viewer():
                return True
            fname = path[len("/api/mel/") : -len("/gt")]
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}") if raw else {}
                if not isinstance(payload, dict):
                    raise hub_mel_gt.MelGtError("object_required")
                info = hub_mel_gt.set_mel_gt(fname, payload.get("gt_label"))
                self._json(200, info)
            except FileNotFoundError as e:
                self._json(404, {"error": "not_found", "file": str(e)})
            except json.JSONDecodeError:
                self._json(400, {"error": "bad_json"})
            except hub_mel_gt.MelGtError as e:
                self._json(400, {"error": str(e)})
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except OSError as e:
                self._json(500, {"error": str(e)})
            return True
        if path in ("/api/mel/clear", "/api/lab/clear_mel"):
            if not self._auth_viewer():
                return True
            info = clear_saved_mel()
            with STATE.lock:
                STATE.last_mel_toast = None
                for n in STATE.nodes.values():
                    n.last_mel = None
            self._json(200, {"status": "ok", **info})
            return True
        if path in ("/api/mel/delete", "/api/mel/delete/"):
            if not self._auth_viewer():
                return True
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}") if raw else {}
                if not isinstance(payload, dict):
                    raise ValueError("object_required")
                fname = str(payload.get("file") or payload.get("name") or "")
                info = delete_saved_mel(fname)
                self._json(200, {"status": "ok", **info})
            except FileNotFoundError as e:
                self._json(404, {"error": "not_found", "file": str(e)})
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except OSError as e:
                self._json(500, {"error": str(e)})
            return True
        return False

