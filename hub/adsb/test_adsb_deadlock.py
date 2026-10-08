#!/usr/bin/env python3
"""Regression: ADSB._lock ↔ STATE.lock deadlock (#hub-stability)."""
from __future__ import annotations

import threading
import time
import unittest

from adsb.service import AdsbService


class AdsbDeadlockTests(unittest.TestCase):
    def test_public_dict_under_state_lock_while_collect_centers(self) -> None:
        """Раньше: poll держал ADSB._lock → extra_centers → STATE.lock;
        dashboard держал STATE.lock → ADSB.public_dict → ADSB._lock."""
        state_lock = threading.Lock()
        adsb = AdsbService(store=None, mqtt_creds=lambda: {}, zones=None)
        adsb.fallback_lol = True
        adsb.site_lat = 55.93
        adsb.site_lon = 36.61
        adsb.enabled = False

        def extra_centers() -> list[tuple[float, float]]:
            with state_lock:
                time.sleep(0.02)
                return [(55.82, 37.35)]

        adsb.set_extra_centers_fn(extra_centers)
        errors: list[str] = []

        def dashboard_loop() -> None:
            try:
                for _ in range(30):
                    with state_lock:
                        adsb.public_dict()
                    time.sleep(0.003)
            except Exception as e:  # noqa: BLE001
                errors.append(f"dashboard:{e}")

        def poll_loop() -> None:
            try:
                for _ in range(30):
                    adsb._collect_fallback_centers()
                    time.sleep(0.003)
            except Exception as e:  # noqa: BLE001
                errors.append(f"poll:{e}")

        t_dash = threading.Thread(target=dashboard_loop, name="dash")
        t_poll = threading.Thread(target=poll_loop, name="poll")
        t_dash.start()
        t_poll.start()
        t_dash.join(timeout=8.0)
        t_poll.join(timeout=8.0)
        self.assertFalse(errors, errors)
        self.assertFalse(t_dash.is_alive(), "dashboard thread hung")
        self.assertFalse(t_poll.is_alive(), "poll thread hung")

    def test_want_running_with_extra_outside_adsb_lock(self) -> None:
        state_lock = threading.Lock()
        adsb = AdsbService(store=None, mqtt_creds=lambda: {}, zones=None)
        adsb.enabled = False
        adsb.fallback_lol = True
        adsb.site_lat = None
        adsb.site_lon = None

        def extra() -> list[tuple[float, float]]:
            with state_lock:
                return [(1.0, 2.0)]

        adsb.set_extra_centers_fn(extra)
        with adsb._lock:
            self.assertFalse(adsb._want_running_unlocked())
        self.assertTrue(adsb._want_running())


if __name__ == "__main__":
    unittest.main()
