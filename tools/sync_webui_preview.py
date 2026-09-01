#!/usr/bin/env python3
"""Sync tools/webui_preview.html from WebUI_page.h + WebUI_i18n.h (RU demo default).

Usage:
  python3 tools/sync_webui_preview.py           # RU → webui_preview.html
  python3 tools/sync_webui_preview.py --lang en # also write webui_preview.en.html
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGE = ROOT / "src" / "WebUI_page.h"
I18N = ROOT / "src" / "WebUI_i18n.h"
OUT = ROOT / "tools" / "webui_preview.html"
OUT_EN = ROOT / "tools" / "webui_preview.en.html"


def unesc(s: str) -> str:
    return bytes(s, "utf-8").decode("unicode_escape") if "\\" in s else s


def i18n_sub(text: str, entries: list[tuple[str, str, str]], *, lang: str) -> str:
    for key, ru, en in entries:
        text = text.replace(f"__T_{key}__", unesc(en if lang == "en" else ru))
    return text


def build_html(lang: str) -> tuple[str, int]:
    page = PAGE.read_text(encoding="utf-8")
    i18n = I18N.read_text(encoding="utf-8")
    m = re.search(r'R"rawliteral\(\n(.*?)\)rawliteral"', page, re.S)
    if not m:
        raise SystemExit("WEBUI_HTML_PAGE rawliteral not found")
    html = m.group(1)
    entries = re.findall(
        r'\{\s*"([^"]+)"\s*,\s*"((?:\\.|[^"\\])*)"\s*,\s*"((?:\\.|[^"\\])*)"\s*\}',
        i18n,
    )
    if not entries:
        raise SystemExit("i18n entries not found")
    html = html.replace("__HTML_LANG__", "en" if lang == "en" else "ru")
    html = i18n_sub(html, entries, lang=lang)

    tls_ca_block = i18n_sub(
        '<div class="panel"><h2>__T_TLS_CA__</h2>'
        '<div class="settings-form">'
        '<div class="stat-row"><span class="stat-label">__T_STATUS__</span>'
        '<span id="tlsCaStatus" class="stat-value">—</span></div>'
        '<label for="tlsCaPem">__T_TLS_CA__</label>'
        '<textarea id="tlsCaPem" rows="4" placeholder="-----BEGIN CERTIFICATE-----" '
        'style="width:100%;font-family:monospace;font-size:12px;background:var(--bg);'
        'border:1px solid var(--border);border-radius:6px;padding:8px;color:var(--text)"></textarea>'
        '<p class="hint">__T_TLS_CA_HINT__</p>'
        '<button type="button" onclick="saveTlsCa()">__T_TLS_CA_SAVE__</button>'
        "</div></div>",
        entries,
        lang=lang,
    )

    html = (
        html.replace("RTSPMIC_PLACEHOLDER_NAME", "MIC DEV")
        .replace("RTSPMIC_PLACEHOLDER_VERSION", "preview")
        .replace("RTSPMIC_PLACEHOLDER_WEBPORT", "80")
        .replace("RTSPMIC_SAMPLE_RATE", "16000")
        .replace("RTSPMIC_BODY_PROFILE", "profile-dev")
        .replace("RTSPMIC_ETH_NET_ROW", "")
        .replace("RTSPMIC_CSRF_TOKEN_JS", "var CSRF_TOKEN='preview';")
        .replace("RTSPMIC_DEFAULT_PW_JS", "var RTSPMIC_DEFAULT_PW=0;")
        .replace("'RTSPMIC_WEB_USER'", "'admin'")
        .replace('"RTSPMIC_WEB_USER"', '"admin"')
        .replace("RTSPMIC_TLS_CA_BLOCK", tls_ca_block)
        .replace("window.RTSPMIC_PREVIEW=0;", "window.RTSPMIC_PREVIEW=1;")
    )
    leftover = [
        p
        for p in (
            "RTSPMIC_CSRF_TOKEN_JS",
            "RTSPMIC_DEFAULT_PW_JS",
            "RTSPMIC_TLS_CA_BLOCK",
            "RTSPMIC_ETH_NET_ROW",
            "RTSPMIC_PLACEHOLDER_NAME",
            "RTSPMIC_PRIVACY_BLOCK",
        )
        if p in html
    ]
    if leftover:
        raise SystemExit(f"placeholders not replaced: {leftover}")
    left = sorted(set(re.findall(r"__T_[A-Z0-9_]+__", html)))
    if left:
        raise SystemExit(f"leftover placeholders: {left}")
    if "window.RTSPMIC_PREVIEW=1;" not in html:
        raise SystemExit("failed to force RTSPMIC_PREVIEW=1")
    header = (
        f"<!-- Auto-synced from WebUI_page.h + WebUI_i18n.h ({lang.upper()}, demo ON). "
        "Rebuild: python tools/sync_webui_preview.py -->\n"
    )
    return header + html, len(entries)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lang", choices=("ru", "en"), default="ru")
    args = ap.parse_args()

    ru_html, n = build_html("ru")
    OUT.write_text(ru_html, encoding="utf-8")
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {n} strings, demo=ON)")

    if args.lang == "en":
        en_html, n_en = build_html("en")
        OUT_EN.write_text(en_html, encoding="utf-8")
        print(f"wrote {OUT_EN} ({OUT_EN.stat().st_size} bytes, {n_en} strings, EN check)")


if __name__ == "__main__":
    main()
