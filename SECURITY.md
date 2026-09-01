# Security · Безопасность

## Reporting

If you find a vulnerability in the **open RTSP Mic** firmware, WebUI, or this repository’s tooling, please open a **private** GitHub security advisory when available, or email the maintainer via the GitHub profile on [Gfermoto/UAV-radar](https://github.com/Gfermoto/UAV-radar).

Do **not** file a public Issue with exploit details.

## Scope

| In scope | Out of scope |
|----------|----------------|
| Open mic firmware / WebUI / MQTT defaults | Closed detection weights inside `nevod-diy-*.bin` (report product issues via Issues without reverse-engineering asks) |
| Docs that leak secrets by mistake | Physical security of a DIY install |

## Defaults

Change default WebUI passwords immediately (`rtsp-mic-change-me` / `nevod-change-me`). Prefer LAN or TLS reverse-proxy for WebUI on untrusted networks. Cloud token is account-scoped — treat it like a password.

## Signed DIY binaries

Public DIY sensor images are published with `.sig` and `manifest.json` (sha256). Verify before flashing — see [docs/DIY_GUIDE.md](docs/DIY_GUIDE.md).
