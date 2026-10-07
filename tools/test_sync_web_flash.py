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
    ESP_BTN_RE,
    FACTORY,
    INDEX,
    MANIFEST,
    FlashSyncError,
    assert_cors_safe_manifest,
    assert_factory_image,
    semver_from_ota_title,
)
from release_page import (  # noqa: E402
    DIY_GUIDE,
    GUIDE_RE,
    README,
    README_TAG_RE,
    RELEASE_DIR,
    ReleasePageError,
    render,
    tag_for,
)


class WebFlashManifestTests(unittest.TestCase):
    def test_semver_from_ota_title(self) -> None:
        self.assertEqual(semver_from_ota_title("nevod-diy-v0.17.2-ota"), "0.17.2")
        self.assertEqual(semver_from_ota_title("v0.17.2-ota"), "0.17.2")
        man = json.loads(MANIFEST.read_text(encoding="utf-8"))
        assert_cors_safe_manifest(man)
        self.assertRegex(man["version"], r"^\d+\.\d+\.\d+$")
        labels = {m.group(0).split()[-1] for m in ESP_BTN_RE.finditer(INDEX.read_text(encoding="utf-8"))}
        self.assertEqual(labels, {man["version"]})
        self.assertEqual(man["new_install_improv_wait_time"], 0)
        self.assertIs(man["new_install_prompt_erase"], True)

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

    def test_rejects_improv_wait_and_auto_erase(self) -> None:
        man = json.loads(MANIFEST.read_text(encoding="utf-8"))
        man["new_install_improv_wait_time"] = 10
        with self.assertRaises(FlashSyncError):
            assert_cors_safe_manifest(man)
        man["new_install_improv_wait_time"] = 0
        man["new_install_prompt_erase"] = False
        with self.assertRaises(FlashSyncError):
            assert_cors_safe_manifest(man)

    def test_factory_image_layout(self) -> None:
        blob = FACTORY.read_bytes()
        assert_factory_image(blob)
        self.assertEqual(blob[2], 0x02)
        self.assertEqual(blob[0x10002], 0x02)
        self.assertGreater(len(blob), 0x10000)


class LandingVersionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.version = json.loads(MANIFEST.read_text(encoding="utf-8"))["version"]

    def test_release_page_exists_for_manifest(self) -> None:
        page = RELEASE_DIR / f"{self.version}.html"
        self.assertTrue(page.is_file(), f"нет {page.relative_to(ROOT)}")
        self.assertIn(f"releases/tag/{tag_for(self.version)}", page.read_text(encoding="utf-8"))

    def test_readme_and_guide_point_to_manifest(self) -> None:
        tags = set(README_TAG_RE.findall(README.read_text(encoding="utf-8")))
        self.assertEqual(tags, {tag_for(self.version)})
        guide = DIY_GUIDE.read_text(encoding="utf-8")
        found = GUIDE_RE.findall(guide)
        self.assertEqual(len(found), 2)
        self.assertEqual(guide.count(f"**{self.version}** · versioned [`{tag_for(self.version)}`]"), 2)

    def test_no_stale_ota_assets_in_pages(self) -> None:
        for name in ("firmware-nevod_diy.manifest.json", "firmware-nevod_diy.bin.sig"):
            self.assertFalse((FACTORY.parent / name).exists(), f"docs/flash/{name}: копия OTA-ассета не для factory")


class ReleasePageRenderTests(unittest.TestCase):
    BODY = (
        "> **Заменён на [v1.2.4](https://example.org/x)**\n\n## OTA\n"
        "- firmware-nevod_diy.bin sha256 " + "a" * 64 + "\n"
        "- model sha256 " + "b" * 64 + "\n"
        "- **Сборка:** `x<y` & пр.\n"
    )

    def test_render_parses_body(self) -> None:
        out = render("1.2.3", self.BODY)
        self.assertIn("a" * 64, out)
        self.assertIn("b" * 64, out)
        self.assertIn("<strong>Сборка:</strong> <code>x&lt;y</code> &amp; пр.", out)
        self.assertIn('<a href="https://example.org/x">v1.2.4</a>', out)
        self.assertIn("releases/tag/nevod-diy-v1.2.3-ota", out)

    def test_render_rejects_body_without_sha(self) -> None:
        with self.assertRaises(ReleasePageError):
            render("1.2.3", "- что-то\n")


if __name__ == "__main__":
    unittest.main()
