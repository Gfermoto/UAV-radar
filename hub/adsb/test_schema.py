"""#113 ADS-B MQTT schema fixtures + validators."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from adsb.schema import (  # noqa: E402
    SCHEMA_AC,
    SCHEMA_HEALTH,
    build_ac,
    build_health,
    normalize_hex,
    topic_ac,
    topic_health,
    topic_snapshot,
    validate_ac,
    validate_health,
    validate_snapshot,
)

FIX = HERE / "fixtures"


class AdsbSchemaTest(unittest.TestCase):
    def test_topics(self) -> None:
        self.assertEqual(topic_health("lab"), "nevod/lab/adsb/health")
        self.assertEqual(topic_ac("lab", "4b9e4a"), "nevod/lab/adsb/ac/4B9E4A")
        self.assertEqual(topic_snapshot("lab"), "nevod/lab/adsb/aircraft")
        with self.assertRaises(ValueError):
            topic_health("../x")

    def test_normalize_hex(self) -> None:
        self.assertEqual(normalize_hex("4b9e4a"), "4B9E4A")
        self.assertEqual(normalize_hex("~abc123"), "ABC123")
        with self.assertRaises(ValueError):
            normalize_hex("zz")

    def test_fixture_health(self) -> None:
        msg = json.loads((FIX / "health.v1.json").read_text())
        validate_health(msg)
        self.assertEqual(msg["schema"], SCHEMA_HEALTH)

    def test_fixture_ac(self) -> None:
        msg = json.loads((FIX / "ac.v1.json").read_text())
        validate_ac(msg)
        self.assertEqual(msg["schema"], SCHEMA_AC)
        self.assertTrue(msg["in_radius"])
        self.assertIn("lat", msg["aircraft"])

    def test_fixture_snapshot(self) -> None:
        msg = json.loads((FIX / "snapshot.v1.json").read_text())
        validate_snapshot(msg)

    def test_builders(self) -> None:
        h = build_health(
            site_id="lab",
            ts="2026-08-12T12:00:00Z",
            total_aircraft=2,
            aircraft_with_positions=1,
            in_radius_count=0,
            adsb_radius_m=30000,
            source_url="http://example/aircraft.json",
            poll_age_s=1.0,
        )
        self.assertTrue(h["ok"])
        ac = build_ac(
            site_id="lab",
            ts="2026-08-12T12:00:00Z",
            aircraft={"hex": "abcdef", "lat": 1.0, "lon": 2.0},
            dist_m=1000.0,
            in_radius=True,
        )
        self.assertEqual(ac["hex"], "ABCDEF")

    def test_health_problem_required(self) -> None:
        with self.assertRaises(ValueError):
            build_health(
                site_id="lab",
                ts="2026-08-12T12:00:00Z",
                total_aircraft=0,
                aircraft_with_positions=0,
                in_radius_count=0,
                adsb_radius_m=30000,
                source_url="http://x",
                poll_age_s=5.0,
                ok=False,
                problem=None,
            )


if __name__ == "__main__":
    unittest.main()
