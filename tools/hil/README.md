# Hardware-in-the-Loop · Проверки на плате

**[English](#english)** · **[Русский](#русский)**

Tests against a **real board** (UART + HTTP).

---

<a id="english"></a>
## English

### Run (Windows COM + LAN)

```powershell
pip install pyserial
py -3 tools/hil/test_device_hil.py --port COM6 --base http://192.168.2.65
```

From WSL if COM is unavailable (HTTP only):

```bash
python3 tools/hil/test_device_hil.py --skip-uart --base http://192.168.2.65
```

### What it checks

| Group | Checks |
|-------|--------|
| UART | boot, WebUI up, no panic / reboot loop |
| HTTP | SPA, `/status` 401/200, live timestamp, CSRF, PWA, default-password gates |
| Both | no Guru Meditation under `/status` load |

### Environment

- `RTSPMIC_HIL_PORT` (default `COM6`)
- `RTSPMIC_HIL_BASE` (default `http://192.168.2.65`)
- `RTSPMIC_HIL_USER` / `RTSPMIC_HIL_PASS` (default `admin` / `rtsp-mic-change-me`)

### Full hardware audit

```powershell
py -3 tools/hil/audit_device_full.py --port COM6 --base http://192.168.2.65 --password <pass>
```

API matrix, WS auth, RTSP OPTIONS, `/status` load, UART panics.

Also useful: `test_rtsp_options.py`, `soak_stability.py` (see repo `tools/hil/`).

---

<a id="русский"></a>
## Русский

Тесты на **живой плате** (UART + HTTP).

### Запуск

Windows — как в английской секции (`COM6` + `http://…`).  
Из WSL без COM: `--skip-uart` и только HTTP.

### Что смотрит

Загрузка и отсутствие panic, SPA/`/status`/CSRF, гейты default-пароля, устойчивость под опросом статуса.

### Переменные окружения

`RTSPMIC_HIL_PORT`, `RTSPMIC_HIL_BASE`, `RTSPMIC_HIL_USER`, `RTSPMIC_HIL_PASS`.

Полный аудит: `audit_device_full.py`. Дополнительно: `test_rtsp_options.py`, `soak_stability.py`.
