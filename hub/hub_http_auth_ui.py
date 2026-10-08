"""HTTP mixin extracted from nevod_hub.Handler — Wave C."""
from __future__ import annotations

from typing import Any

class HubAuthUiHttp:
    def _handle_ui_login(self, path: str) -> bool:
        """Public setup/login/reset/logout + password change. True if handled."""
        ui_paths = {
            "/api/hub/ui/setup",
            "/api/hub/ui/setup/",
            "/api/hub/ui/login",
            "/api/hub/ui/login/",
            "/api/hub/ui/reset",
            "/api/hub/ui/reset/",
            "/api/hub/ui/logout",
            "/api/hub/ui/logout/",
            "/api/hub/ui/password",
            "/api/hub/ui/password/",
        }
        if path not in ui_paths:
            return False
        if path.rstrip("/").endswith("logout"):
            tok = _ui_token_from_cookie(self.headers.get("Cookie"))
            if tok:
                _ui_session_drop(tok)
            self._json(
                200,
                {"status": "ok"},
                set_cookie=_ui_clear_cookie_header(),
                cors=False,
            )
            return True
        raw = self._read_body()
        if raw is None:
            return True
        try:
            payload = json.loads(raw.decode("utf-8") or "{}") if raw else {}
        except json.JSONDecodeError:
            self._json(400, {"error": "bad_json"}, cors=False)
            return True
        if not isinstance(payload, dict):
            self._json(400, {"error": "object_required"}, cors=False)
            return True
        user = str(payload.get("user") or payload.get("username") or "")
        password = str(payload.get("password") or "")
        ingest = str(payload.get("ingest_token") or payload.get("token") or "")

        if path.rstrip("/").endswith("password"):
            if not self._auth_ui_cookie():
                self._json(401, {"error": "unauthorized"}, cors=False)
                return True
            current = str(payload.get("current_password") or "")
            stored_u = ""
            if STORE is not None:
                v = STORE.get_meta("ui_user")
                if isinstance(v, str):
                    stored_u = v
            if not _ui_verify(stored_u, current):
                self._json(403, {"error": "bad_login"}, cors=False)
                return True
            new_user = user.strip() or stored_u
            new_pw = password
            try:
                _ui_set(new_user, new_pw)
            except ValueError as e:
                self._json(400, {"error": str(e)}, cors=False)
                return True
            sess = _ui_issue()
            self._json(
                200,
                {"status": "ok", "user": new_user},
                set_cookie=_ui_set_cookie_header(sess),
                cors=False,
            )
            return True

        if path.rstrip("/").endswith("setup"):
            if _ui_configured():
                self._json(409, {"error": "already_configured"}, cors=False)
                return True
            try:
                _validate_ui_user(user)
                _validate_ui_password(password)
                _ui_set(user, password)
            except ValueError as e:
                self._json(400, {"error": str(e)}, cors=False)
                return True
            sess = _ui_issue()
            self._json(
                200,
                {"status": "ok", "configured": True},
                set_cookie=_ui_set_cookie_header(sess),
                cors=False,
            )
            return True

        if path.rstrip("/").endswith("reset"):
            self._json(
                410,
                {
                    "error": "reset_disabled",
                    "detail": "Публичный сброс отключён. Восстановление — SSH на VPS (см. README_HUB).",
                },
                cors=False,
            )
            return True

        # login
        if not _ui_configured():
            self._json(409, {"error": "not_configured"}, cors=False)
            return True
        if not _ui_verify(user, password):
            self._json(401, {"error": "bad_login"}, cors=False)
            return True
        sess = _ui_issue()
        self._json(
            200,
            {"status": "ok"},
            set_cookie=_ui_set_cookie_header(sess),
            cors=False,
        )
        return True

