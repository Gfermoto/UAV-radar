#!/usr/bin/env python3
"""Собрать factory-образ для ESP Web Tools на GitHub Pages.

GitHub Releases не отдаёт CORS — manifest должен ссылаться на same-origin файл.
OTA asset `firmware-nevod_diy.bin` — приложение; для USB 0x0 нужен merge.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLASH = ROOT / "docs" / "flash"
PARTS = FLASH / "parts"
MANIFEST = FLASH / "manifest.json"
FACTORY = FLASH / "firmware-nevod_diy.bin"
FACTORY_REL = "firmware-nevod_diy.bin"
SEMVER_RE = re.compile(
    r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
)


class FlashSyncError(RuntimeError):
    """Fail-closed web-flash sync error."""


def semver_from_ota_title(name: str) -> str:
    match = SEMVER_RE.search(name or "")
    if match is None:
        raise FlashSyncError(f"no semver in OTA title: {name!r}")
    return match.group(0)


def assert_cors_safe_manifest(man: dict) -> None:
    if man.get("name") != "NEVOD DIY":
        raise FlashSyncError("manifest.name must be NEVOD DIY")
    builds = man.get("builds")
    if not isinstance(builds, list) or not builds:
        raise FlashSyncError("manifest.builds missing")
    for build in builds:
        if build.get("chipFamily") != "ESP32-S3":
            raise FlashSyncError("chipFamily must be ESP32-S3")
        for part in build.get("parts") or []:
            path = part.get("path", "")
            if not isinstance(path, str) or not path:
                raise FlashSyncError("empty part path")
            if "://" in path or path.startswith("/") or "github.com" in path:
                raise FlashSyncError(
                    f"part path must be same-origin relative, got {path!r}"
                )
            if path != FACTORY_REL:
                raise FlashSyncError(f"unexpected part path {path!r}")
            if int(part.get("offset", -1)) != 0:
                raise FlashSyncError("factory image offset must be 0")


def assert_factory_image(blob: bytes) -> None:
    if len(blob) < 0x10010:
        raise FlashSyncError(f"factory image too small: {len(blob)}")
    if blob[0] != 0xE9:
        raise FlashSyncError("missing bootloader magic at 0x0")
    if blob[2] != 0x02:
        raise FlashSyncError(
            f"bootloader flash mode must be DIO (2), got {blob[2]}"
        )
    if blob[0x8000] != 0xAA:
        raise FlashSyncError("missing partition magic at 0x8000")
    if blob[0x10000] != 0xE9:
        raise FlashSyncError("missing app magic at 0x10000")
    if blob[0x10002] != 0x02:
        raise FlashSyncError(
            f"app flash mode must be DIO (2), got {blob[0x10002]}"
        )


def merge_factory(app: Path, out: Path) -> None:
    boot = PARTS / "bootloader.bin"
    parts = PARTS / "partitions.bin"
    boot_app0 = PARTS / "boot_app0.bin"
    for p in (app, boot, parts, boot_app0):
        if not p.is_file() or p.stat().st_size < 16:
            raise FlashSyncError(f"missing flash part: {p}")
    cmd = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "merge-bin",
        "--flash-mode",
        "dio",
        "--flash-freq",
        "80m",
        "--flash-size",
        "8MB",
        "-o",
        str(out),
        "0x0",
        str(boot),
        "0x8000",
        str(parts),
        "0xe000",
        str(boot_app0),
        "0x10000",
        str(app),
    ]
    subprocess.run(cmd, check=True)
    assert_factory_image(out.read_bytes())


def write_manifest(version: str) -> None:
    man = {
        "name": "NEVOD DIY",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": [{"path": FACTORY_REL, "offset": 0}],
            }
        ],
    }
    assert_cors_safe_manifest(man)
    MANIFEST.write_text(json.dumps(man, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True, help="OTA app .bin")
    parser.add_argument("--version", required=True, help="semver without -ota")
    args = parser.parse_args()
    merge_factory(args.app.expanduser().resolve(), FACTORY)
    write_manifest(args.version.strip())
    print(f"wrote {FACTORY} ({FACTORY.stat().st_size} bytes) version={args.version}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FlashSyncError as exc:
        print(f"sync_web_flash: {exc}", file=sys.stderr)
        raise SystemExit(1)
