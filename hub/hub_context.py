"""HubContext — composition bundle for Nevod Hub services.

Handlers should read services via ``handler.ctx`` (attached to the HTTP server),
not via module globals. Module globals remain as a shim for self-test / legacy
imports until Wave F cleanup.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class HubContext:
    state: Any = None
    mqtt: Any = None
    store: Any = None
    webhooks: Any = None
    forwarder: Any = None
    adsb: Any = None
    zones: Any = None
    device_tokens: dict[str, str] = field(default_factory=dict)
    cert_path: Path | None = None
    require_device_token: bool = False
    dashboard_open: bool = False

    def sync_module(self, mod: Any) -> None:
        """Push fields onto a composition-root module (nevod_hub globals shim)."""
        mod.STATE = self.state
        mod.MQTT = self.mqtt
        mod.STORE = self.store
        mod.WEBHOOKS = self.webhooks
        mod.FORWARDER = self.forwarder
        mod.ADSB = self.adsb
        mod.ZONES = self.zones
        mod.DEVICE_TOKENS = self.device_tokens
        mod.CERT_PATH = self.cert_path
        mod.REQUIRE_DEVICE_TOKEN = bool(self.require_device_token)
        mod.DASHBOARD_OPEN = bool(self.dashboard_open)

    @classmethod
    def from_module(cls, mod: Any) -> "HubContext":
        """Capture live module globals (self-test after it assigns STATE/…)."""
        tokens = getattr(mod, "DEVICE_TOKENS", None)
        if not isinstance(tokens, dict):
            tokens = {}
        return cls(
            state=getattr(mod, "STATE", None),
            mqtt=getattr(mod, "MQTT", None),
            store=getattr(mod, "STORE", None),
            webhooks=getattr(mod, "WEBHOOKS", None),
            forwarder=getattr(mod, "FORWARDER", None),
            adsb=getattr(mod, "ADSB", None),
            zones=getattr(mod, "ZONES", None),
            device_tokens=tokens,
            cert_path=getattr(mod, "CERT_PATH", None),
            require_device_token=bool(getattr(mod, "REQUIRE_DEVICE_TOKEN", False)),
            dashboard_open=bool(getattr(mod, "DASHBOARD_OPEN", False)),
        )

    def bind_adsb_zones(self) -> None:
        """Zones → ADSB feeder bind (single chokepoint for admin POSTs)."""
        if self.adsb is not None and self.zones is not None:
            bind = getattr(self.adsb, "bind_zones", None)
            if callable(bind):
                bind(self.zones)
