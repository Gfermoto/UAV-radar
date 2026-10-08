#!/usr/bin/env python3
"""adsb.lol normalize / blind / URL (no network)."""
from __future__ import annotations

import unittest

from adsb.lol_fallback import (
    build_lol_url,
    is_blind_aircraft_json,
    normalize_lol_to_aircraft_json,
)


class LolFallbackTests(unittest.TestCase):
    def test_normalize_ac_key(self) -> None:
        raw = normalize_lol_to_aircraft_json(
            {
                "now": 1.5,
                "ac": [
                    {
                        "hex": "ABC123",
                        "lat": 55.8,
                        "lon": 37.3,
                        "flight": " AFL123 ",
                        "alt_baro": 3000,
                    }
                ],
            }
        )
        self.assertEqual(raw["source"], "adsb.lol")
        self.assertEqual(len(raw["aircraft"]), 1)
        self.assertEqual(raw["aircraft"][0]["hex"], "abc123")
        self.assertEqual(raw["aircraft"][0]["flight"], "AFL123")

    def test_blind_empty(self) -> None:
        self.assertTrue(is_blind_aircraft_json({"aircraft": []}))
        self.assertTrue(is_blind_aircraft_json({"aircraft": [{"hex": "a"}]}))
        self.assertFalse(
            is_blind_aircraft_json({"aircraft": [{"hex": "a", "lat": 1.0, "lon": 2.0}]})
        )

    def test_url(self) -> None:
        u = build_lol_url(55.82, 37.35, 80)
        self.assertIn("api.adsb.lol/v2/lat/55.82000/lon/37.35000/dist/80", u)


if __name__ == "__main__":
    unittest.main()
