#!/usr/bin/env python3
"""Страница релиза docs/release/X.Y.Z.html и ссылки на текущий DIY-релиз.

Источник — тело GitHub-релиза `nevod-diy-vX.Y.Z-ota` (markdown):
строки `- firmware-nevod_diy.bin sha256 …`, `- model sha256 …`,
остальные пункты `- …` — «Что нового», цитата `> …` — примечание.
Существующую страницу не перезаписывает без --force (ручные правки живут).
"""
from __future__ import annotations

import argparse
import html
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RELEASE_DIR = ROOT / "docs" / "release"
README = ROOT / "README.md"
DIY_GUIDE = ROOT / "docs" / "DIY_GUIDE.md"
SITE = "https://gfermoto.github.io/UAV-radar"
REPO = "https://github.com/Gfermoto/UAV-radar"
SEMVER = r"\d+\.\d+\.\d+"
README_TAG_RE = re.compile(rf"nevod-diy-v{SEMVER}-ota")
GUIDE_RE = re.compile(
    rf"((?:сейчас|currently) \*\*){SEMVER}(\*\* · versioned \[`)[^`]+(`\]\({re.escape(REPO)}/releases/tag/)[^)]+(\))"
)
BIN_SHA_RE = re.compile(r"^firmware-nevod_diy\.bin sha256 ([0-9a-f]{64})$")
MODEL_SHA_RE = re.compile(r"^model sha256 ([0-9a-f]{64})$")
LEAD = "Подписанный бинарник для USB-прошивки (XIAO ESP32-S3 + Seeed XVF3800)."


class ReleasePageError(RuntimeError):
    """Fail-closed release page error."""


def tag_for(version: str) -> str:
    return f"nevod-diy-v{version}-ota"


def _inline(md: str) -> str:
    out = html.escape(md, quote=False)
    out = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", out)
    out = re.sub(r"`([^`]+)`", r"<code>\1</code>", out)
    return re.sub(r"\[([^\]]+)\]\((https://[^)\s]+)\)", r'<a href="\2">\1</a>', out)


def _plain(md: str) -> str:
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", md)
    return re.sub(r"[*`>]", "", text).strip()


def parse_body(body: str) -> dict:
    bin_sha = model_sha = None
    notes: list[str] = []
    items: list[str] = []
    for raw in body.splitlines():
        line = raw.strip()
        if line.startswith(">"):
            notes.append(line.lstrip("> ").strip())
        elif line.startswith("- "):
            item = line[2:].strip()
            if m := BIN_SHA_RE.match(item):
                bin_sha = m.group(1)
            elif m := MODEL_SHA_RE.match(item):
                model_sha = m.group(1)
            else:
                items.append(item)
    if not bin_sha:
        raise ReleasePageError("в теле релиза нет строки firmware-nevod_diy.bin sha256")
    if not items:
        raise ReleasePageError("в теле релиза нет пунктов «Что нового»")
    return {"bin_sha": bin_sha, "model_sha": model_sha, "notes": notes, "items": items}


def render(version: str, body: str) -> str:
    rel = parse_body(body)
    title = f"NEVOD DIY v{version}-ota"
    url = f"{SITE}/release/{version}.html"
    desc = _plain(rel["items"][0])
    if len(desc) > 160:
        desc = desc[:157].rstrip() + "…"
    desc = html.escape(f"NEVOD DIY {version}-ota: {desc}")
    notes = "".join(f'    <p class="lead">{_inline(n)}</p>\n' for n in rel["notes"])
    items = "".join(f"        <li>{_inline(i)}</li>\n" for i in rel["items"])
    model = (
        f"        <li>Model sha256: <code>{rel['model_sha']}</code></li>\n" if rel["model_sha"] else ""
    )
    return f"""<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{title} · UAV-radar</title>
  <meta name="description" content="{desc}">
  <link rel="canonical" href="{url}">
  <meta property="og:type" content="article">
  <meta property="og:site_name" content="UAV-radar">
  <meta property="og:title" content="{title}">
  <meta property="og:description" content="{desc}">
  <meta property="og:url" content="{url}">
  <meta property="og:image" content="{SITE}/assets/og-default.png">
  <meta name="twitter:card" content="summary_large_image">
  <link rel="stylesheet" href="../assets/site.css">
</head>
<body>
  <main class="wrap">
    <div class="hero">
      <img src="../assets/og-default.png" width="807" height="450" alt="{title}">
    </div>
    <h1>{title}</h1>
    <p class="lead">{LEAD}</p>
{notes}
    <div class="card">
      <h2>Что нового</h2>
      <ul>
{items}      </ul>
    </div>

    <div class="card">
      <h2>Скачать</h2>
      <ul>
        <li>Bin sha256: <code>{rel['bin_sha']}</code></li>
{model}        <li>Файлы: <code>firmware-nevod_diy.bin</code>, <code>.sig</code>, <code>.manifest.json</code></li>
      </ul>
      <div class="btn-row">
        <a class="btn" href="{REPO}/releases/tag/{tag_for(version)}">Скачать на GitHub</a>
        <a class="btn secondary" href="{SITE}/">Прошить с лендинга</a>
      </div>
    </div>

    <p class="footer"><a href="../">← UAV-radar</a> · <a href="{REPO}">GitHub</a> · <a href="{REPO}/releases/tag/diy-ota">канал diy-ota</a></p>
  </main>
</body>
</html>
"""


def write_page(version: str, body: str, *, force: bool = False) -> Path:
    page = RELEASE_DIR / f"{version}.html"
    if page.exists() and not force:
        return page
    page.write_text(render(version, body), encoding="utf-8")
    return page


def bump_current_links(version: str) -> None:
    tag = tag_for(version)
    readme = README.read_text(encoding="utf-8")
    new, n = README_TAG_RE.subn(tag, readme)
    if n == 0:
        raise ReleasePageError("README.md: нет ссылок nevod-diy-vX.Y.Z-ota")
    README.write_text(new, encoding="utf-8")
    guide = DIY_GUIDE.read_text(encoding="utf-8")
    new, n = GUIDE_RE.subn(lambda m: f"{m.group(1)}{version}{m.group(2)}{tag}{m.group(3)}{tag}{m.group(4)}", guide)
    if n == 0:
        raise ReleasePageError("DIY_GUIDE.md: нет строки «сейчас **X.Y.Z** · versioned»")
    DIY_GUIDE.write_text(new, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="semver without -ota")
    parser.add_argument("--body-file", type=Path, required=True, help="тело GitHub-релиза (markdown)")
    parser.add_argument("--force", action="store_true", help="перезаписать существующую страницу")
    args = parser.parse_args()
    version = args.version.strip()
    if not re.fullmatch(SEMVER, version):
        raise ReleasePageError(f"не semver: {version!r}")
    page = write_page(version, args.body_file.read_text(encoding="utf-8"), force=args.force)
    bump_current_links(version)
    print(f"release page {page.relative_to(ROOT)}; README/DIY_GUIDE → {tag_for(version)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ReleasePageError as exc:
        print(f"release_page: {exc}", file=sys.stderr)
        raise SystemExit(1)
