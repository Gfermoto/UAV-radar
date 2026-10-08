"""HTTP mixin extracted from nevod_hub.Handler — Wave C."""
from __future__ import annotations

from typing import Any

class HubMqttHttp:
    def _handle_mqtt_post(self, path: str) -> bool:
        """MQTT connect/disconnect POSTs. True if handled."""
        if path not in ("/api/mqtt", "/api/mqtt/", "/api/mqtt/disconnect"):
            return False
        if not self._auth_viewer():
            return True
        assert MQTT is not None
        raw = self._read_body()
        if raw is None:
            return True
        try:
            payload = json.loads(raw.decode("utf-8") or "{}") if raw else {}
            if not isinstance(payload, dict):
                raise ValueError("object_required")
            if path.endswith("/disconnect"):
                out = MQTT.apply({"enabled": False}, connect=False)
            else:
                out = MQTT.apply(payload)
            self._json(200, {"status": "ok", "mqtt": out})
        except ValueError as e:
            self._json(400, {"error": str(e), "accepted": False})
        except Exception as e:  # noqa: BLE001
            self._json(500, {"error": str(e), "accepted": False})
        return True

