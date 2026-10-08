"""HTTP mixin extracted from nevod_hub.Handler — Wave C."""
from __future__ import annotations

from typing import Any

class HubAdminHttp:
    def _handle_hub_admin(self, path: str) -> bool:
        """Hub cloud/webhooks/settings/adsb/device_token POSTs. True if handled."""
        assert STATE is not None
        nid_path = parse_hub_node_label_path(path)
        if nid_path is not None:
            if not self._auth_viewer():
                return True
            if STORE is None:
                self._json(503, {"error": "store_unavailable"})
                return True
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                if not isinstance(payload, dict):
                    raise ValueError("object_required")
                if "label" not in payload:
                    raise ValueError("label_required")
                out = apply_node_label(STORE, nid_path, payload["label"])
                self._json(200, {"status": "ok", **out})
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except Exception:  # noqa: BLE001
                self._json(500, {"error": "internal_error"})
            return True
        if path in ("/api/hub/device_token", "/api/hub/device_token/"):
            if not self._auth():
                return True
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                nid = str(payload.get("node_id") or "").strip()
                tok = str(payload.get("token") or "").strip()
                if not nid or not tok:
                    raise ValueError("node_id_and_token_required")
                DEVICE_TOKENS[nid] = tok
                if STORE is not None:
                    STORE.set_device_token(nid, tok)
                self._json(200, {"status": "ok", "node_id": nid})
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except Exception as e:  # noqa: BLE001
                self._json(500, {"error": str(e)})
            return True
        if path in ("/api/hub/cloud/token", "/api/hub/cloud/token/"):
            if not self._auth_viewer():
                return True
            if FORWARDER is None:
                self._json(503, {"error": "forwarder_unavailable"})
                return True
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                if not isinstance(payload, dict):
                    raise ValueError("object_required")
                tok = str(payload.get("token") or "").strip()
                if not tok:
                    raise ValueError("token_required")
                if tok.startswith("Bearer "):
                    tok = tok[7:].strip()
                out = FORWARDER.set_token(tok, probe=True)
                self._json(200, {"status": "ok", "forward": out})
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except Exception as e:  # noqa: BLE001
                self._json(500, {"error": str(e)})
            return True
        if path in ("/api/hub/cloud/unlock/set", "/api/hub/cloud/unlock/set/"):
            if not self._auth_viewer():
                return True
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                pw = str(payload.get("password") or "")
                _unlock_set(pw)
                sess = _unlock_issue()
                self._json(
                    200,
                    {
                        "status": "ok",
                        "unlock_set": True,
                        "unlocked": True,
                        "token": sess,
                    },
                )
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except Exception as e:  # noqa: BLE001
                self._json(500, {"error": str(e)})
            return True
        if path in ("/api/hub/cloud/unlock", "/api/hub/cloud/unlock/"):
            if not self._auth_viewer():
                return True
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                pw = str(payload.get("password") or "")
                stored = _unlock_hash_stored()  # meta → env → lab hardcoded (opt-in)
                if not stored:
                    self._json(403, {"error": "unlock_not_configured"})
                    return True
                if not hmac.compare_digest(_hash_unlock_pw(pw), stored):
                    self._json(403, {"error": "bad_unlock_password"})
                    return True
                sess = _unlock_issue()
                fwd = (
                    FORWARDER.public_dict(include_origin=True)
                    if FORWARDER
                    else {}
                )
                self._json(
                    200,
                    {
                        "status": "ok",
                        "unlocked": True,
                        "token": sess,
                        "forward": fwd,
                    },
                )
            except Exception as e:  # noqa: BLE001
                self._json(500, {"error": str(e)})
            return True
        if path in ("/api/hub/cloud/origin", "/api/hub/cloud/origin/"):
            if not self._auth():
                return True
            if not _unlock_session_ok(self.headers.get("X-Hub-Unlock")):
                self._json(403, {"error": "cloud_locked"})
                return True
            if FORWARDER is None:
                self._json(503, {"error": "forwarder_unavailable"})
                return True
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                if not isinstance(payload, dict):
                    raise ValueError("object_required")
                base = str(
                    payload.get("base")
                    or payload.get("url")
                    or payload.get("origin")
                    or ""
                ).strip()
                if base and not (
                    base.startswith("https://") or base.startswith("http://")
                ):
                    raise ValueError("origin_must_be_http_s")
                ca = payload.get("ca_file")
                mel = payload.get("forward_mel")
                out = FORWARDER.set_origin(
                    base=base,
                    ca_file=None if ca is None else str(ca),
                    forward_mel=None if mel is None else bool(mel),
                )
                self._json(200, {"status": "ok", "forward": out})
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except Exception as e:  # noqa: BLE001
                self._json(500, {"error": str(e)})
            return True
        if path in ("/api/hub/webhooks", "/api/hub/webhooks/"):
            if not self._auth_viewer():
                return True
            if WEBHOOKS is None:
                self._json(503, {"error": "webhooks_unavailable"})
                return True
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                if not isinstance(payload, dict):
                    raise ValueError("object_required")
                out = WEBHOOKS.apply_config(payload, persist=True)
                self._json(200, {"status": "ok", "webhooks": out})
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except Exception as e:  # noqa: BLE001
                self._json(500, {"error": str(e)})
            return True
        if path in ("/api/hub/settings/unlock", "/api/hub/settings/unlock/"):
            self._json(410, {"error": "gone", "hint": "use_engineer_unlock"})
            return True
        if path in ("/api/hub/settings/password", "/api/hub/settings/password/"):
            self._json(410, {"error": "gone", "hint": "use_ui_password"})
            return True
        if path in ("/api/hub/adsb", "/api/hub/adsb/"):
            if not self._auth_viewer():
                return True
            if ADSB is None:
                self._json(503, {"error": "adsb_unavailable"})
                return True
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                if not isinstance(payload, dict):
                    raise ValueError("object_required")
                out = ADSB.apply(payload, persist=True)
                self._json(200, {"status": "ok", "adsb": out})
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except Exception as e:  # noqa: BLE001
                self._json(500, {"error": str(e)})
            return True
        if path in ("/api/hub/zones", "/api/hub/zones/"):
            if not self._auth_viewer():
                return True
            if ZONES is None:
                self._json(503, {"error": "zones_unavailable"})
                return True
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                if not isinstance(payload, dict):
                    raise ValueError("object_required")
                # SSRF-check feeder URLs before persist
                for item in payload.get("feeders") or []:
                    if isinstance(item, dict) and item.get("url"):
                        assert_adsb_url_allowed(str(item["url"]).strip())
                out = ZONES.apply_patch(payload, persist=True)
                if ADSB is not None:
                    self.ctx.bind_adsb_zones()
                    if ZONES.enabled_feeders_unique():
                        ADSB.start()
                self._json(200, {"status": "ok", "zones": out})
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except Exception as e:  # noqa: BLE001
                self._json(500, {"error": str(e)})
            return True
        if path in ("/api/hub/map_wx", "/api/hub/map_wx/"):
            if not self._auth_viewer():
                return True
            if STORE is None:
                self._json(503, {"error": "store_unavailable"})
                return True
            raw = self._read_body()
            if raw is None:
                return True
            try:
                payload = json.loads(raw.decode("utf-8") or "{}")
                if not isinstance(payload, dict):
                    raise ValueError("object_required")
                touched = False
                configured = bool(str(STORE.get_meta("open_meteo_api_key") or "").strip())
                hint = ""
                if configured:
                    k0 = str(STORE.get_meta("open_meteo_api_key") or "")
                    hint = ("…" + k0[-4:]) if len(k0) >= 4 else "set"
                eco_url_out = str(STORE.get_meta("ecowitt_share_url") or "").strip()
                eco_r = 8.0
                try:
                    eco_r = float(STORE.get_meta("ecowitt_radius_km") or 8.0)
                except (TypeError, ValueError):
                    eco_r = 8.0

                if "api_key" in payload:
                    key = str(payload.get("api_key") or "").strip()
                    if key.lower() in ("", "clear", "-"):
                        STORE.set_meta("open_meteo_api_key", "")
                        configured = False
                        hint = ""
                    else:
                        if len(key) < 8:
                            raise ValueError("key_too_short")
                        STORE.set_meta("open_meteo_api_key", key)
                        configured = True
                        hint = "…" + key[-4:]
                    touched = True

                if "ecowitt_share_url" in payload:
                    eu = str(payload.get("ecowitt_share_url") or "").strip()
                    if eu.lower() in ("", "clear", "-"):
                        STORE.set_meta("ecowitt_share_url", "")
                        eco_url_out = ""
                    else:
                        if parse_share_url(eu) is None:
                            raise ValueError("bad_ecowitt_url")
                        STORE.set_meta("ecowitt_share_url", eu)
                        eco_url_out = eu
                    clear_ecowitt_cache()
                    touched = True

                if "ecowitt_radius_km" in payload:
                    try:
                        eco_r = float(payload.get("ecowitt_radius_km"))
                    except (TypeError, ValueError) as e:
                        raise ValueError("bad_ecowitt_radius") from e
                    if not (1.0 <= eco_r <= 50.0):
                        raise ValueError("bad_ecowitt_radius")
                    STORE.set_meta("ecowitt_radius_km", str(eco_r))
                    touched = True

                if not touched:
                    raise ValueError("nothing_to_save")

                try:
                    from hub_meteo import clear_cache

                    clear_cache()
                except Exception:
                    pass
                eco_ok = parse_share_url(eco_url_out) is not None
                self._json(
                    200,
                    {
                        "status": "ok",
                        "configured": configured,
                        "key_hint": hint,
                        "via": "settings" if configured else "none",
                        "ecowitt_share_url": eco_url_out if eco_ok else "",
                        "ecowitt_configured": eco_ok,
                        "ecowitt_radius_km": eco_r,
                    },
                )
            except ValueError as e:
                self._json(400, {"error": str(e)})
            except Exception as e:  # noqa: BLE001
                self._json(500, {"error": str(e)})
            return True
        return False

