#!/usr/bin/env python3
"""Guard: landing Web Serial manifest stays same-origin (no GitHub Releases)."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from sync_web_flash import (  # noqa: E402
    FACTORY,
    MANIFEST,
    FlashSyncError,
    assert_cors_safe_manifest,
    assert_factory_image,
    semver_from_ota_title,
)


class WebFlashManifestTests(unittest.TestCase):
    def test_semver_from_ota_title(self) -> None:
        self.assertEqual(semver_from_ota_title("nevod-diy-v0.17.2-ota"), "0.17.2")
        self.assertEqual(semver_from_ota_title("v0.17.2-ota"), "0.17.2")
        man = json.loads(MANIFEST.read_text(encoding="utf-8"))
        assert_cors_safe_manifest(man)
        self.assertEqual(man["version"], "0.17.2")

    def test_rejects_github_release_url(self) -> None:
        man = {
            "name": "NEVOD DIY",
            "version": "0.17.2",
            "builds": [
                {
                    "chipFamily": "ESP32-S3",
                    "parts": [
                        {
                            "path": "https://github.com/Gfermoto/UAV-radar/releases/download/diy-ota/firmware-nevod_diy.bin",
                            "offset": 0,
                        }
                    ],
                }
            ],
        }
        with self.assertRaises(FlashSyncError):
            assert_cors_safe_manifest(man)

    def test_factory_image_layout(self) -> None:
        blob = FACTORY.read_bytes()
        assert_factory_image(blob)
        self.assertGreater(len(blob), 0x10000)


if __name__ == "__main__":
    unittest.main()
