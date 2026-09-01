# Проверки на плате · Hardware-in-the-Loop

**[Русский](#русский)** · **[English](#english)**

Скрипты для **живой платы** (UART + HTTP) — после сборки и прошивки открытого mic.

---

<a id="русский"></a>
## Русский

### Запуск (Windows COM + LAN)

```powershell
pip install pyserial
py -3 tools/hil/test_device_hil.py --port COM6 --base http://192.168.2.65
```

Из WSL без COM (только HTTP):

```bash
python3 tools/hil/test_device_hil.py --skip-uart --base http://192.168.2.65
```

### Что проверяется

| Группа | Проверки |
|--------|----------|
| UART | загрузка, WebUI, без panic / reboot loop |
| HTTP | SPA, `/status` 401/200, живый timestamp, CSRF, PWA, гейты default-пароля |
| Совместно | нет Guru Meditation под нагрузкой `/status` |

### Переменные окружения

- `RTSPMIC_HIL_PORT` (по умолчанию `COM6`)
- `RTSPMIC_HIL_BASE` (по умолчанию `http://192.168.2.65`)
- `RTSPMIC_HIL_USER` / `RTSPMIC_HIL_PASS` (по умолчанию `admin` / `rtsp-mic-change-me`)

### Полный аудит

```powershell
py -3 tools/hil/audit_device_full.py --port COM6 --base http://192.168.2.65 --password <pass>
```

Матрица API, WS auth, RTSP OPTIONS, нагрузка `/status`, UART panics.

Дополнительно: `test_rtsp_options.py`, `soak_stability.py` в `tools/hil/`.

---

<a id="english"></a>
## English

### Run (Windows COM + LAN)

```powershell
pip install pyserial
py -3 tools/hil/test_device_hil.py --port COM6 --base http://192.168.2.65
```

From WSL without COM (HTTP only):

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

Also: `test_rtsp_options.py`, `soak_stability.py` in `tools/hil/`.
